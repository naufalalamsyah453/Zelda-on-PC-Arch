#include "wxgui/CemuUpdateWindow.h"

#include "Common/version.h"
#include "util/helpers/helpers.h"
#include "util/helpers/SystemException.h"
#include "util/helpers/ZipArchive.h"
#include "config/ActiveSettings.h"
#include "Common/FileStream.h"
#include "wxCemuConfig.h"
#include "CemuApp.h"

#include <wx/sizer.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>

#ifndef BOOST_OS_WINDOWS
#include <unistd.h>
#include <sys/stat.h>
#endif

#include <curl/curl.h>
#include <boost/tokenizer.hpp>
#include <openssl/rand.h>
#include <rapidjson/document.h>
#include <wx/utils.h>
#include "pcedition/PCEdition.h"


wxDECLARE_EVENT(wxEVT_RESULT, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_RESULT, wxCommandEvent);

wxDECLARE_EVENT(wxEVT_PROGRESS, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_PROGRESS, wxCommandEvent);

CemuUpdateWindow::CemuUpdateWindow(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Cemu update"), wxDefaultPosition, wxDefaultSize,
		wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX)
{
	auto* sizer = new wxBoxSizer(wxVERTICAL);
	m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(500, 20), wxGA_HORIZONTAL);
	m_gauge->SetValue(0);
	sizer->Add(m_gauge, 0, wxALL | wxEXPAND, 5);

	auto* rows = new wxFlexGridSizer(0, 2, 0, 0);
	rows->AddGrowableCol(1);

	m_text = new wxStaticText(this, wxID_ANY, _("Checking for latest version..."));
	rows->Add(m_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	{
		auto* right_side = new wxBoxSizer(wxHORIZONTAL);

		m_updateButton = new wxButton(this, wxID_ANY, _("Update"));
		m_updateButton->Bind(wxEVT_BUTTON, &CemuUpdateWindow::OnUpdateButton, this);
		right_side->Add(m_updateButton, 0, wxALL, 5);

		m_cancelButton = new wxButton(this, wxID_ANY, _("Cancel"));
		m_cancelButton->Bind(wxEVT_BUTTON, &CemuUpdateWindow::OnCancelButton, this);
		right_side->Add(m_cancelButton, 0, wxALL, 5);

		rows->Add(right_side, 1, wxALIGN_RIGHT, 5);
	}

	m_changelog = new wxHyperlinkCtrl(this, wxID_ANY, _("Changelog"), wxEmptyString);
	rows->Add(m_changelog, 0, wxLEFT | wxBOTTOM | wxRIGHT | wxEXPAND, 5);

	sizer->Add(rows, 0, wxALL | wxEXPAND, 5);

	SetSizerAndFit(sizer);
	Centre(wxBOTH);

	Bind(wxEVT_CLOSE_WINDOW, &CemuUpdateWindow::OnClose, this);
	Bind(wxEVT_RESULT, &CemuUpdateWindow::OnResult, this);
	Bind(wxEVT_PROGRESS, &CemuUpdateWindow::OnGaugeUpdate, this);
	m_thread = std::thread(&CemuUpdateWindow::WorkerThread, this);

	m_updateButton->Hide();
	m_changelog->Hide();
}

CemuUpdateWindow::~CemuUpdateWindow()
{
	m_order = WorkerOrder::Exit;
	if (m_thread.joinable())
		m_thread.join();
}

size_t CemuUpdateWindow::WriteStringCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	((std::string*)userdata)->append(ptr, size * nmemb);
	return size * nmemb;
};

static bool HexToBytes(const std::string& hex, std::vector<uint8>& dataOut)
{
	if (hex.size() % 2)
		return false;
	dataOut.clear();
	dataOut.reserve(hex.size() / 2);
	for (size_t i = 0; i < hex.size(); i += 2)
	{
		unsigned int x;
		auto [p, ec] = std::from_chars(hex.data() + i, hex.data() + i + 2, x, 16);
		if (ec != std::errc{} || p != hex.data() + i + 2)
			return false;
		dataOut.push_back(static_cast<uint8>(x));
	}
	return true;
}

// --- helpers for the release-feed based update check --------------------------------

static size_t FetchWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	((std::string*)userdata)->append(ptr, size * nmemb);
	return size * nmemb;
}

// GET url into out; false on a transport error or a non-200 response
static bool FetchUrlToString(const std::string& url, std::string& out)
{
	out.clear();
	CURL* curl = curl_easy_init();
	if (!curl)
		return false;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, FetchWriteCallback);
	// GitHub's API refuses requests without a user agent
	curl_easy_setopt(curl, CURLOPT_USERAGENT, BUILD_VERSION_WITH_NAME_STRING);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
	const CURLcode cr = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(curl);
	return cr == CURLE_OK && (http_code == 200 || http_code == 0);
}

static bool EndsWith(const std::string& s, const std::string& suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// "v1.4.2" against "v1.4.1": is the candidate a strictly newer release than the current one?
static bool IsVersionNewer(const std::string& candidate, const std::string& current)
{
	auto parse = [](const std::string& v) {
		std::array<int, 3> r{ 0, 0, 0 };
		size_t pos = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
		for (int i = 0; i < 3 && pos < v.size(); i++)
		{
			size_t end = pos;
			while (end < v.size() && v[end] >= '0' && v[end] <= '9')
				end++;
			if (end == pos)
				break;
			r[i] = std::atoi(v.substr(pos, end - pos).c_str());
			pos = end + 1; // skip the separator
		}
		return r;
	};
	return parse(candidate) > parse(current);
}

// returns true if a newer release than the running build exists on the project's release page
// and fills in the Arch package to install, the release page and the package's sha256
bool CemuUpdateWindow::QueryUpdateInfo(std::string& downloadUrlOut, std::string& changelogUrlOut, std::array<uint8, 32>& fileSha256Out)
{
	static const char* kLatestRelease = "https://api.github.com/repos/naufalalamsyah453/Zelda-on-PC-Arch/releases/latest";

	std::string json;
	if (!FetchUrlToString(kLatestRelease, json))
	{
		cemuLog_log(LogType::Force, "Update check failed (release feed unreachable)");
		return false;
	}

	rapidjson::Document d;
	d.Parse(json.data(), json.size());
	if (d.HasParseError() || !d.IsObject())
	{
		cemuLog_log(LogType::Force, "Update check: release feed is not valid JSON");
		return false;
	}

	const std::string tag = (d.HasMember("tag_name") && d["tag_name"].IsString()) ? d["tag_name"].GetString() : "";
	if (tag.empty() || !IsVersionNewer(tag, pce::kEditionVersion))
		return false; // up to date (or running a newer build than the feed offers)

	std::string pkgUrl, shaUrl, releasePage;
	if (d.HasMember("html_url") && d["html_url"].IsString())
		releasePage = d["html_url"].GetString();
	if (d.HasMember("assets") && d["assets"].IsArray())
	{
		for (const auto& asset : d["assets"].GetArray())
		{
			if (!asset.IsObject() || !asset.HasMember("name") || !asset["name"].IsString() ||
				!asset.HasMember("browser_download_url") || !asset["browser_download_url"].IsString())
				continue;
			const std::string name = asset["name"].GetString();
			const std::string url = asset["browser_download_url"].GetString();
			if (EndsWith(name, ".pkg.tar.zst.sha256"))
				shaUrl = url;
			else if (EndsWith(name, ".pkg.tar.zst"))
				pkgUrl = url;
		}
	}
	if (pkgUrl.empty() || shaUrl.empty())
	{
		cemuLog_log(LogType::Force, "Update check: release {} carries no Arch package", tag);
		return false;
	}

	// the published .sha256 sidecar: "<hex digest>  <file name>"
	std::string shaText;
	if (!FetchUrlToString(shaUrl, shaText))
	{
		cemuLog_log(LogType::Force, "Update check: could not fetch the package checksum");
		return false;
	}
	const std::string digest = shaText.substr(0, shaText.find_first_of(" \t\r\n"));
	std::vector<uint8> bytes;
	if (!HexToBytes(digest, bytes) || bytes.size() != 32)
	{
		cemuLog_log(LogType::Force, "Update check: published checksum is not a sha256 digest");
		return false;
	}

	downloadUrlOut = pkgUrl;
	changelogUrlOut = releasePage;
	std::memcpy(fileSha256Out.data(), bytes.data(), 32);
	return true;
}

std::future<bool> CemuUpdateWindow::IsUpdateAvailableAsync()
{
	return std::async(std::launch::async, CheckVersion);
}

bool CemuUpdateWindow::CheckVersion()
{
	std::string downloadUrl, changelogUrl;
	std::array<uint8, 32> fileSha256;
	return QueryUpdateInfo(downloadUrl, changelogUrl, fileSha256);
}


int CemuUpdateWindow::ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	auto* thisptr = (CemuUpdateWindow*)clientp;
	auto* event = new wxCommandEvent(wxEVT_PROGRESS);
	event->SetInt((int)dlnow);
	wxQueueEvent(thisptr, event);
	return 0;
}

void CemuUpdateWindow::SubmitWorkerResult(Result newWorkerState)
{
	auto* event = new wxCommandEvent(wxEVT_RESULT);
	event->SetInt((int)newWorkerState);
	wxQueueEvent(this, event);
}

static std::atomic<bool> s_cemuUpdateCancelRequest{false};

bool CemuUpdateWindow::DownloadCemuUpdateFile(const std::string& url, const fs::path& filename)
{
	if (!fs::exists(filename.parent_path()))
		fs::create_directories(filename.parent_path());
	auto* curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, BUILD_VERSION_WITH_NAME_STRING);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
	auto r = curl_easy_perform(curl);
	if (r != CURLE_OK)
	{
		cemuLog_log(LogType::Force, "Cemu update download failed with error {}", r);
		curl_easy_cleanup(curl);
		return false;
	}
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	if (http_code != 0 && http_code != 200)
	{
		cemuLog_log(LogType::Force, "Unable to download cemu update file from {} (http error: {})", url, http_code);
		curl_easy_cleanup(curl);
		return false;
	}

	curl_off_t update_size;
	if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &update_size) == CURLE_OK)
		m_gaugeMaxValue = (int)update_size;

	std::vector<uint8> fileBuffer;

	s_cemuUpdateCancelRequest = false;
	auto _curlWriteData = +[](void* ptr, size_t size, size_t nmemb, void* ctx) -> size_t
	{
		if (s_cemuUpdateCancelRequest)
			return 0;
		std::vector<uint8>* fileBufferVec = (std::vector<uint8>*)ctx;
		const size_t writeSize = size * nmemb;
		fileBufferVec->insert(fileBufferVec->end(), (uint8*)ptr, (uint8*)ptr + writeSize);
		return writeSize;
	};

	curl_easy_setopt(curl, CURLOPT_NOBODY, 0);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curlWriteData);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fileBuffer);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);

	auto curl_result = std::async(std::launch::async, [](CURL* curl, long* http_code)
		{
			const auto r = curl_easy_perform(curl);
			return r;
		}, curl, &http_code);
	while (true)
	{
		auto r = curl_result.wait_for(std::chrono::milliseconds(100));
		if (r == std::future_status::ready)
			break;
		if (m_order == WorkerOrder::Exit && !s_cemuUpdateCancelRequest)
			s_cemuUpdateCancelRequest = true;
	}
	curl_easy_cleanup(curl);
	CURLcode curlResult = curl_result.get();
	if (curlResult == CURLE_ABORTED_BY_CALLBACK)
	{
		return false;
	}
	else if (curlResult != CURLE_OK)
	{
		return false;
	}
	// store file to temp location
	std::error_code ec;
	fs::remove(filename, ec);
	if (!FileStream::WriteFileAtomic(filename, fileBuffer))
	{
		cemuLog_log(LogType::Force, "Failed to store update file");
		return false;
	}
	// read back file and verify the hash
	auto diskFileData = FileStream::LoadIntoMemory(filename);
	if (!diskFileData)
	{
		cemuLog_log(LogType::Force, "Failed to read back update file");
		return false;
	}
	// verify the file hash
	std::array<uint8_t, 32> hash;
	EVP_Q_digest(nullptr, "SHA256", nullptr, diskFileData->data(), diskFileData->size(), hash.data(), nullptr);
	if (hash != m_fileSHA256)
	{
		cemuLog_log(LogType::Force, "File hash of downloaded update does not match");
		fs::remove(filename, ec);
		return false;
	}
	SubmitWorkerResult(Result::UpdateDownloaded);
	return true;
}

bool CemuUpdateWindow::ExtractZipUpdate(const fs::path& zipname, const fs::path& targetpath)
{
	ZipArchive archive(zipname);
	if (!archive.IsValid())
	{
		cemuLog_log(LogType::Force, "Cannot open zip file: {}", _pathToUtf8(zipname));
		return false;
	}
	const size_t count = archive.GetNumEntries();
	m_gaugeMaxValue = count;
	// the update files are in a directory called "Cemu_<version>". Find the name of the directory
	std::string cemuPathPrefix;
	for (size_t i=0; i<count; i++)
	{
		ZipArchive::ZipFileEntry entry;
		if (!archive.GetFileEntryByIndex(i, entry))
			continue;
		std::string_view firstDir = std::string_view(entry.fullPath).substr(0, entry.fullPath.find_first_of('/'));
		if (!firstDir.empty() && firstDir.length() != entry.fullPath.length() && firstDir.starts_with("Cemu_"))
		{
			if (!cemuPathPrefix.empty() && cemuPathPrefix != firstDir)
			{
				cemuLog_log(LogType::Force, "Cemu update zip contains multiple Cemu directories in root? Found {} and {}", firstDir, cemuPathPrefix);
				return false;
			}
			cemuPathPrefix = firstDir;
		}
	}
	if (cemuPathPrefix.empty())
	{
		cemuLog_log(LogType::Force, "Cemu update zip does not match expected structure");
		return false;
	}
	if (m_order == WorkerOrder::Exit)
		return false;
	std::string cemuPathPrefxWithSlash = cemuPathPrefix + "/";
	// extract each file's data first to force CRC checks and we create the folders already
	// this acts as a basic permission check too
	for (size_t i = 0; i < count; i++)
	{
		ZipArchive::ZipFileEntry entry;
		if (archive.GetFileEntryByIndex(i, entry))
		{
			if (!entry.fullPath.starts_with(cemuPathPrefxWithSlash))
				continue; // ignore files outside the directory, there shouldn't be any but future releases might add additional files so we'll allow it just in case
			const fs::path entryPath = _utf8ToPath(entry.fullPath.substr(cemuPathPrefxWithSlash.length()));
			// read full file into memory
			std::vector<uint8> fileData;
			if (!archive.ExtractIntoMemoryByIndex(i, fileData))
			{
				cemuLog_log(LogType::Force, "Failed to extract file {}", entry.fullPath);
				return false;
			}
			// make sure the folder exists
			const fs::path fname = targetpath / entryPath;
			try
			{
				fs::create_directories(fname.parent_path());
			}
			catch (const std::exception& ex)
			{
				SystemException sys(ex);
				cemuLog_log(LogType::Force, "can't create folder for update file \"{}\": {}", entry.fullPath, sys.what());
				return false;
			}
		}
	}
	// check if there is enough disk space (to keep this simple we assume 100MB, generally releases should be below that)
	std::error_code ec;
	const fs::space_info targetSpace = fs::space(ActiveSettings::GetExecutablePath().parent_path(), ec);
	if (targetSpace.free <= 100 * 1024 * 1024)
	{
		cemuLog_log(LogType::Force, "Not enough disk space available for update");
		return false;
	}
	// actually extract and replace files
	for (size_t i = 0; i < count; i++)
	{
		ZipArchive::ZipFileEntry entry;
		if (archive.GetFileEntryByIndex(i, entry))
		{
			if (!entry.fullPath.starts_with(cemuPathPrefxWithSlash))
				continue;
			const fs::path entryPath = _utf8ToPath(entry.fullPath.substr(cemuPathPrefxWithSlash.length()));
			// read into memory
			std::vector<uint8> fileData;
			if (!archive.ExtractIntoMemoryByIndex(i, fileData))
			{
				cemu_assert(false); // extraction tested earlier, it should not fail here
			}
			const fs::path fname = targetpath / entryPath;
			if ( !FileStream::WriteFileAtomic(fname, fileData, true) )
			{
				cemuLog_log(LogType::Force, "Failed to update {}", _pathToUtf8(fname));
				// continue regardless
			}
			if ((i / 10) * 10 == i)
			{
				auto* event = new wxCommandEvent(wxEVT_PROGRESS);
				event->SetInt(static_cast<int>(i));
				wxQueueEvent(this, event);
			}
		}
	}

	auto* event = new wxCommandEvent(wxEVT_PROGRESS);
	event->SetInt(m_gaugeMaxValue);
	wxQueueEvent(this, event);

	return true;
}

#if BOOST_OS_WINDOWS
bool CemuUpdateWindow::WorkerThread_Windows()
{
	const auto tmppath = fs::temp_directory_path() / L"cemu_update";
	std::error_code ec;
	if (exists(tmppath))
		remove_all(tmppath, ec);
	const auto updateSrcFile = tmppath / L"update.zip";
	if (!DownloadCemuUpdateFile(m_downloadUrl, updateSrcFile))
		return false;
	if (m_order == WorkerOrder::Exit)
		return false;
	// extract and replace files
	if (!ExtractZipUpdate(updateSrcFile, ActiveSettings::GetExecutablePath().parent_path()))
	{
		cemuLog_log(LogType::Force, "Extracting Cemu zip failed");
		return false;
	}
	SubmitWorkerResult(Result::ExtractSuccess);
	// set relaunch path
	fs::path newExePath = ActiveSettings::GetExecutablePath();
	newExePath = newExePath.parent_path().append("Cemu.exe");
	m_restartFile = newExePath;
	return true;
}
#endif

#if BOOST_OS_LINUX
bool CemuUpdateWindow::WorkerThread_AppImage()
{
	const auto tmppath = fs::temp_directory_path() / L"cemu_update";
	std::error_code ec;
	if (exists(tmppath, ec))
		remove_all(tmppath, ec);
	const auto updateSrcFile = tmppath / L"Cemu.AppImage";
	if (!DownloadCemuUpdateFile(m_downloadUrl, updateSrcFile))
		return false;
	if (m_order == WorkerOrder::Exit)
		return false;
	const char* appimage_path = std::getenv("APPIMAGE");
	auto backupExecutable = fs::path(appimage_path);
	backupExecutable.replace_extension( _utf8ToPath(_pathToUtf8(backupExecutable.extension()).append(".backup")));
	const char* filePath = updateSrcFile.c_str();
	mode_t permissions = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
	fs::rename(appimage_path, backupExecutable, ec);
	if (ec)
	{
		cemuLog_log(LogType::Force, "Failed to rename current .AppImage for update replacement");
		return false;
	}
	m_restartFile = appimage_path;
	chmod(filePath, permissions);
	wxString wxAppPath = wxString::FromUTF8(appimage_path);
	wxCopyFile (wxString::FromUTF8(_pathToUtf8(updateSrcFile)), wxAppPath);
	return true;
}

bool CemuUpdateWindow::WorkerThread_Pacman()
{
	// download the Arch package (verified against the sha256 the release published), then hand it
	// to the system package manager; pkexec raises the admin password prompt
	const auto tmppath = fs::temp_directory_path() / L"zelda-on-pc-update";
	std::error_code ec;
	if (exists(tmppath, ec))
		remove_all(tmppath, ec);
	const fs::path pkgFile = tmppath / L"update.pkg.tar.zst";
	if (!DownloadCemuUpdateFile(m_downloadUrl, pkgFile))
		return false;
	if (m_order == WorkerOrder::Exit)
		return false;

	// "Applying update..." and the cancel button disabled while the package manager runs
	SubmitWorkerResult(Result::ExtractSuccess);

	// --noconfirm skips pacman's second "proceed with installation?" prompt, because pkexec
	// already authenticated the user (the temp path is quoted in case the directory has spaces)
	wxString cmd = "pkexec /usr/bin/pacman -U --noconfirm \"";
	cmd += wxString::FromUTF8(_pathToUtf8(pkgFile));
	cmd += "\"";
	const long ret = wxExecute(cmd, wxEXEC_SYNC);

	fs::remove_all(tmppath, ec);
	if (ret != 0)
	{
		cemuLog_log(LogType::Force, "Installing the update failed (exit code {})", ret);
		return false;
	}

	// the package replaced the read-only copy in /opt; restart through the launcher so it copies
	// the new binary into the per-user run directory and starts the new version
	const fs::path launcher = "/usr/bin/zelda-on-pc";
	m_restartFile = fs::exists(launcher) ? launcher : ActiveSettings::GetExecutablePath();
	return true;
}
#endif

#if BOOST_OS_MACOS
bool CemuUpdateWindow::WorkerThread_MacBundle()
{
	const fs::path tempPath = fs::temp_directory_path() / "cemu_update";

    std::error_code ec;
    fs::remove_all(tempPath, ec);
    fs::create_directories(tempPath, ec);
    if (ec)
        return false;

    const fs::path dmgPath = tempPath / "cemu.dmg";
    if (!DownloadCemuUpdateFile(m_downloadUrl, dmgPath))
        return false;

    if (m_order == WorkerOrder::Exit)
        return false;

    // .../Cemu.app/Contents/MacOS/Cemu
    const fs::path executable = ActiveSettings::GetExecutablePath();
    const fs::path appBundle = executable.parent_path().parent_path().parent_path();
    if (appBundle.extension() != ".app")
    {
        cemuLog_log(LogType::Force, "Cemu is not running from a macOS app bundle");
        return false;
    }

    const fs::path scriptPath = tempPath / "update.sh";
    const fs::path mountPath = tempPath / "mount";

    auto ShellQuote = [](std::string_view value)
    {
        std::string result = "'";
        for (const char c : value)
        {
            if (c == '\'')
                result += "'\\''";
            else
                result += c;
        }
        result += "'";
        return result;
    };

    std::string dmg = ShellQuote(_pathToUtf8(dmgPath));
    std::string app = ShellQuote(_pathToUtf8(appBundle));
    std::string mount = ShellQuote(_pathToUtf8(mountPath));

	std::string script;

	script += "#!/bin/sh\n";
	script += "DMG_PATH=" + ShellQuote(_pathToUtf8(dmgPath)) + "\n";
	script += "APP_BUNDLE=" + ShellQuote(_pathToUtf8(appBundle)) + "\n";
	script += "MOUNT_PATH=" + ShellQuote(_pathToUtf8(mountPath)) + "\n";

	script += R"SH(
set -eu

STAGED_BUNDLE="${APP_BUNDLE}.update"
BACKUP_BUNDLE="${APP_BUNDLE}.backup"
MOUNTED=0

finish()
{
    STATUS=$?
    trap - EXIT HUP INT TERM

    if [ "$MOUNTED" -eq 1 ]; then
        /usr/bin/hdiutil detach "$MOUNT_PATH" >/dev/null 2>&1 || true
    fi

    if [ -d "$APP_BUNDLE" ]; then
        /usr/bin/open -n "$APP_BUNDLE" || true
    elif [ -x "$BACKUP_BUNDLE/Contents/MacOS/Cemu" ]; then
        "$BACKUP_BUNDLE/Contents/MacOS/Cemu" &
    fi

    exit "$STATUS"
}

trap finish EXIT HUP INT TERM

/bin/rm -rf "$MOUNT_PATH" "$STAGED_BUNDLE"
/bin/mkdir -p "$MOUNT_PATH"

/usr/bin/hdiutil attach \
    -nobrowse \
    -readonly \
    -mountpoint "$MOUNT_PATH" \
    "$DMG_PATH"

MOUNTED=1

/usr/bin/ditto \
    "$MOUNT_PATH/Cemu.app" \
    "$STAGED_BUNDLE"

/usr/bin/codesign \
    --verify \
    --strict \
    "$STAGED_BUNDLE/Contents/MacOS/Cemu"

/usr/bin/hdiutil detach "$MOUNT_PATH"
MOUNTED=0

/bin/rm -rf "$BACKUP_BUNDLE"
/bin/mv "$APP_BUNDLE" "$BACKUP_BUNDLE"

if ! /bin/mv "$STAGED_BUNDLE" "$APP_BUNDLE"; then
    /bin/mv "$BACKUP_BUNDLE" "$APP_BUNDLE" || true
    exit 1
fi
)SH";

    std::vector<uint8> scriptData(script.begin(), script.end());
    if (!FileStream::WriteFileAtomic(scriptPath, scriptData))
    {
        cemuLog_log(LogType::Force, "Failed to create macOS updater script");
        return false;
    }
    if (chmod(scriptPath.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) != 0)
    {
        cemuLog_log(LogType::Force, "Failed to make updater script executable");
        return false;
    }
    // run the script on exit
    m_restartFile = scriptPath;
    return true;
}
#endif

void CemuUpdateWindow::WorkerThread()
{
	// start writing log.txt if not created yet
	cemuLog_createLogFile(false);

	while (true)
	{
		std::unique_lock lock(m_mutex);
		WorkerOrder nextWorkOrder = WorkerOrder::Idle;
		while (true)
		{
			nextWorkOrder = m_order.exchange(WorkerOrder::Idle);
			if (nextWorkOrder != WorkerOrder::Idle)
				break;
			m_condition.wait_for(lock, std::chrono::milliseconds(125));
		}
		if (nextWorkOrder == WorkerOrder::Exit)
			break;
		if (nextWorkOrder == WorkerOrder::CheckVersion)
		{
			auto* event = new wxCommandEvent(wxEVT_RESULT);
			if (QueryUpdateInfo(m_downloadUrl, m_changelogUrl, m_fileSHA256))
				event->SetInt((int)Result::UpdateAvailable);
			else
				event->SetInt((int)Result::NoUpdateAvailable);

			wxQueueEvent(this, event);
		}
		else if (nextWorkOrder == WorkerOrder::UpdateVersion)
		{
			// download update
			const std::string url = m_downloadUrl;
			bool r = false;
#if BOOST_OS_WINDOWS
			r = WorkerThread_Windows();
#elif BOOST_OS_LINUX
			r = std::getenv("APPIMAGE") ? WorkerThread_AppImage() : WorkerThread_Pacman();
#elif BOOST_OS_BSD
			// dummy placeholder on BSD for now
#elif BOOST_OS_MACOS
			r = WorkerThread_MacBundle();
#endif
			// update done
			if (r)
			{
				// report success
				auto* event = new wxCommandEvent(wxEVT_PROGRESS);
				event->SetInt(m_gaugeMaxValue);
				wxQueueEvent(this, event);

				auto* result_event = new wxCommandEvent(wxEVT_RESULT);
				result_event->SetInt((int)Result::Success);
				wxQueueEvent(this, result_event);
			}
			else if (m_order != WorkerOrder::Exit)
			{
				SubmitWorkerResult(Result::UpdateDownloadError);
			}
		}
	}
}

void CemuUpdateWindow::OnClose(wxCloseEvent& event)
{
	event.Skip();
	if (m_restartRequired)
	{
		wxGetApp().RequestRestart(m_restartFile);
	}
}


void CemuUpdateWindow::OnResult(wxCommandEvent& event)
{
	switch ((Result)event.GetInt())
	{
	case Result::NoUpdateAvailable:
		m_cancelButton->SetLabel(_("Exit"));
		m_text->SetLabel(_("No update available!"));
		m_gauge->SetValue(100);
		break;
	case Result::UpdateAvailable:
	{
		if (!m_changelogUrl.empty())
		{
			m_changelog->SetURL(m_changelogUrl);
			m_changelog->Show();
		}
		else
			m_changelog->Hide();

		m_updateButton->Show();

		m_text->SetLabel(_("Update available!"));
		m_cancelButton->SetLabel(_("Exit"));
		break;
	}
	case Result::UpdateDownloaded:
		m_text->SetLabel(_("Extracting update..."));
		m_gauge->SetValue(0);
		break;
	case Result::UpdateDownloadError:
		m_updateButton->Enable();
		m_cancelButton->Enable();
		m_text->SetLabel(_("Couldn't download the update!"));
		break;
	case Result::ExtractSuccess:
		m_text->SetLabel(_("Applying update..."));
		m_gauge->SetValue(0);
		m_cancelButton->Disable();
		break;
	case Result::ExtractError:
		m_updateButton->Enable();
		m_cancelButton->Enable();
		m_text->SetLabel(_("Extracting failed!"));
		break;
	case Result::Success:
		m_cancelButton->Enable();
		m_updateButton->Hide();

		m_text->SetLabel(_("Success"));
		m_cancelButton->SetLabel(_("Restart"));
		m_restartRequired = true;
		break;
	default:;
	}
}

void CemuUpdateWindow::OnGaugeUpdate(wxCommandEvent& event)
{
	const int total_size = m_gaugeMaxValue > 0 ? m_gaugeMaxValue : 10000000;
	m_gauge->SetValue((event.GetInt() * 100) / total_size);
}

void CemuUpdateWindow::OnUpdateButton(const wxCommandEvent& event)
{
	std::unique_lock lock(m_mutex);
	m_order = WorkerOrder::UpdateVersion;

	m_condition.notify_all();

	m_updateButton->Disable();

	m_text->SetLabel(_("Downloading update..."));
}

void CemuUpdateWindow::OnCancelButton(const wxCommandEvent& event)
{
	Close();
}
