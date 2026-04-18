// ==========================================
//      FDX - FAD DOWNLOADER-X (MASTER BUILD)
//      Fixes: Network Drops, Wi-Fi Switches & Persistent History
// ==========================================

#define _CRT_SECURE_NO_WARNINGS 

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wldap32.lib")
#pragma comment(lib, "Normaliz.lib") 
#pragma comment(linker, "/SUBSYSTEM:WINDOWS") 

#include <winsock2.h> 
#include <windows.h> 
#include <shlobj.h> // REQUIRED for Persistent History Directory
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/artprov.h>
#include <wx/filedlg.h> 
#include <wx/clipbrd.h> 
#include <wx/filefn.h> 
#include <wx/utils.h> 
#include <wx/progdlg.h> 
#include <wx/snglinst.h> 
#include <curl/curl.h>
#include <thread>
#include <vector>
#include <atomic>
#include <string>
#include <iomanip>
#include <sstream>
#include <mutex>
#include <deque>
#include <numeric>
#include <fstream>
#include <algorithm>
#include <cstdio> 

#ifdef new
#undef new
#endif

const int NUM_THREADS = 8;
const int SPEED_SAMPLES = 10;
const size_t RAM_BUFFER_SIZE = 2 * 1024 * 1024;
static std::atomic<int> g_id_counter(1);

bool g_is_dark_mode = false;
HANDLE g_InstanceMutex = NULL;

std::string GetQueuePath() {
    char tempPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);
    return std::string(tempPath) + "fdx_queue.txt";
}

// --- HISTORY ENGINE ---
std::string GetHistoryPath() {
    char appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appData))) {
        std::string dir = std::string(appData) + "\\FDX";
        CreateDirectoryA(dir.c_str(), NULL);
        return dir + "\\history.dat";
    }
    return GetQueuePath() + "_hist.dat";
}

std::vector<std::string> split_str(const std::string& s, char delim) {
    std::vector<std::string> result; std::stringstream ss(s); std::string item;
    while (getline(ss, item, delim)) result.push_back(item);
    return result;
}

bool IsDownloadableLink(const std::string& text) {
    if (text.find("http") != 0) return false;
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    const char* exts[] = { ".zip", ".rar", ".7z", ".mp4", ".mkv", ".avi", ".pdf", ".iso", ".exe", ".msi", ".m3u8", ".mp3", ".jpg", ".png", ".bin" };
    for (const char* ext : exts) { if (lower.find(ext) != std::string::npos) return true; }
    return false;
}

struct ThreadState { long long start, end, current; };

class DownloadTask {
public:
    int id; std::string url; std::string filename; std::string short_name; long long size = 0;
    std::atomic<long long> downloaded{ 0 }; int active_threads = NUM_THREADS;
    std::atomic<bool> is_running{ false }; std::atomic<bool> is_paused{ false }; std::atomic<bool> is_completed{ false }; std::atomic<bool> error{ false };
    std::string status_text = "Pending..."; long long last_bytes = 0; std::deque<double> speed_history; double current_speed = 0.0;
    std::vector<ThreadState> thread_states; std::vector<std::thread> workers;
    DownloadTask(int _id, std::string _url) : id(_id), url(_url) {}
};

std::vector<std::shared_ptr<DownloadTask>> all_tasks;
std::mutex tasks_mutex; std::mutex disk_mutex;

void SaveHistory() {
    std::lock_guard<std::mutex> lock(tasks_mutex);
    std::ofstream out(GetHistoryPath(), std::ios::trunc);
    if (!out.is_open()) return;
    for (auto& t : all_tasks) {
        out << t->id << "\t" << t->url << "\t" << t->filename << "\t" << t->short_name << "\t"
            << t->size << "\t" << t->downloaded << "\t" << t->is_completed << "\t" << t->status_text << "\n";
    }
}

void LoadHistory(wxListCtrl* listView) {
    std::ifstream in(GetHistoryPath());
    if (!in.is_open()) return;
    std::string line; int max_id = 1000;
    std::lock_guard<std::mutex> lock(tasks_mutex);
    while (std::getline(in, line)) {
        auto parts = split_str(line, '\t');
        if (parts.size() >= 8) {
            int id = std::stoi(parts[0]); auto task = std::make_shared<DownloadTask>(id, parts[1]);
            task->filename = parts[2]; task->short_name = parts[3]; task->size = std::stoll(parts[4]);
            task->downloaded = std::stoll(parts[5]); task->is_completed = (parts[6] == "1"); task->status_text = parts[7];

            if (task->is_completed) task->status_text = "Complete";
            else if (task->status_text == "Downloading" || task->status_text == "Pending..." || task->status_text == "Allocating Disk...") {
                task->status_text = "Paused"; task->is_paused = true;
            }
            if (!task->is_completed && task->size > 0) {
                std::ifstream idm(task->filename + ".idm");
                if (idm.is_open()) {
                    std::string dummy; std::getline(idm, dummy); std::getline(idm, dummy); std::getline(idm, dummy);
                    std::getline(idm, dummy); task->active_threads = std::stoi(dummy);
                    for (int i = 0; i < task->active_threads; i++) {
                        ThreadState ts; idm >> ts.start >> ts.end >> ts.current; task->thread_states.push_back(ts);
                    }
                }
                else { task->downloaded = 0; task->thread_states.clear(); }
            }
            all_tasks.push_back(task); if (id > max_id) max_id = id;
            long index = listView->InsertItem(listView->GetItemCount(), std::to_string(id));
            listView->SetItem(index, 1, task->short_name); listView->SetItem(index, 2, task->size > 0 ? std::to_string(task->size / 1048576) + " MB" : "Unknown");
            listView->SetItem(index, 3, task->status_text);
            if (task->size > 0) { double pct = (double)task->downloaded / task->size * 100.0; listView->SetItem(index, 4, std::to_string((int)pct) + "%"); }
        }
    }
    g_id_counter = max_id + 1;
}

std::string format_bytes(long long bytes) {
    if (bytes >= 1073741824) return std::to_string(bytes / 1073741824) + "." + std::to_string((bytes % 1073741824) / 107374182) + " GB";
    if (bytes >= 1048576) return std::to_string(bytes / 1048576) + "." + std::to_string((bytes % 1048576) / 104857) + " MB";
    if (bytes >= 1024) return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes) + " B";
}

std::string format_time(long long total_seconds) {
    if (total_seconds <= 0) return "Calculating...";
    long long hours = total_seconds / 3600; long long minutes = (total_seconds % 3600) / 60; long long seconds = total_seconds % 60;
    if (hours > 0) return std::to_string(hours) + " hr " + std::to_string(minutes) + " min";
    if (minutes > 0) return std::to_string(minutes) + " min " + std::to_string(seconds) + " sec";
    return std::to_string(seconds) + " sec";
}

void setup_curl(CURL* curl) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/121.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA); curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512 * 1024L); curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);

    // FIX: Severs the connection if Wi-Fi drops and hits 0 bytes/s for 15 seconds (prevents freezing)
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
}

struct FileInfo { long long size = 0; std::string filename = ""; std::string content_type = ""; bool supports_resume = true; };

static size_t info_header_parser(char* buffer, size_t size, size_t nitems, void* userdata) {
    std::string header(buffer, size * nitems); FileInfo* info = (FileInfo*)userdata; std::string lower = header;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("content-disposition:") != std::string::npos && lower.find("filename=") != std::string::npos) {
        size_t pos = lower.find("filename="); std::string raw = header.substr(pos + 9);
        if (raw.front() == '"') raw = raw.substr(1);
        while (!raw.empty() && (raw.back() == '"' || raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' ')) raw.pop_back();
        info->filename = raw;
    }
    if (lower.find("content-type:") != std::string::npos) {
        size_t pos = lower.find(":"); if (pos != std::string::npos) { std::string ct = header.substr(pos + 1); ct.erase(0, ct.find_first_not_of(" \t\r\n")); ct.erase(ct.find_last_not_of(" \t\r\n") + 1); info->content_type = ct; }
    }
    if (lower.find("accept-ranges: none") != std::string::npos) info->supports_resume = false;
    if (lower.find("content-range:") != std::string::npos) {
        size_t slash = lower.find("/"); if (slash != std::string::npos) { std::string total = header.substr(slash + 1); total.erase(std::remove(total.begin(), total.end(), '\r'), total.end()); total.erase(std::remove(total.begin(), total.end(), '\n'), total.end()); try { info->size = std::stoll(total); } catch (...) {} }
    }
    return size * nitems;
}

static size_t probe_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) { return 0; }

FileInfo GetRobustFileInfo(const std::string& url) {
    FileInfo info; info.supports_resume = true; CURL* curl = curl_easy_init(); if (!curl) return info;
    setup_curl(curl); curl_easy_setopt(curl, CURLOPT_URL, url.c_str()); curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, info_header_parser); curl_easy_setopt(curl, CURLOPT_HEADERDATA, &info);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, probe_write_cb); curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_perform(curl); long http_code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 206 && http_code != 200) {
        info.supports_resume = false; curl_easy_setopt(curl, CURLOPT_RANGE, NULL); curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code == 200) { curl_off_t cl = 0; curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl); if (cl > 0) info.size = (long long)cl; }
    }
    else if (http_code == 200) {
        info.supports_resume = false; curl_off_t cl = 0; curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl); if (cl > 0) info.size = (long long)cl;
    }
    char* ct_ptr = NULL; curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct_ptr); if (ct_ptr) info.content_type = ct_ptr;

    char* eff_url_ptr = NULL; curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff_url_ptr);
    std::string final_url = eff_url_ptr ? eff_url_ptr : url; curl_easy_cleanup(curl);

    if (info.filename.empty() || info.filename == "download.bin" || info.filename == "download") {
        size_t slash = final_url.find_last_of("/"); std::string name = "download";
        if (slash != std::string::npos) { name = final_url.substr(slash + 1); size_t qm = name.find("?"); if (qm != std::string::npos) name = name.substr(0, qm); }
        if (name.length() > 50 || name.find(".") == std::string::npos) {
            std::string ext = ".bin"; std::string lower_ct = info.content_type; std::transform(lower_ct.begin(), lower_ct.end(), lower_ct.begin(), ::tolower);
            if (lower_ct.find("video/mp4") != std::string::npos) ext = ".mp4"; else if (lower_ct.find("video/x-matroska") != std::string::npos) ext = ".mkv";
            else if (lower_ct.find("video/webm") != std::string::npos) ext = ".webm"; else if (lower_ct.find("application/zip") != std::string::npos || lower_ct.find("application/x-zip") != std::string::npos) ext = ".zip";
            else if (lower_ct.find("application/pdf") != std::string::npos) ext = ".pdf"; else if (lower_ct.find("application/x-msdownload") != std::string::npos) ext = ".exe";
            else if (lower_ct.find("audio/") != std::string::npos) ext = ".mp3"; else if (lower_ct.find("image/jpeg") != std::string::npos) ext = ".jpg";
            else if (lower_ct.find("image/png") != std::string::npos) ext = ".png";
            if (!name.empty() && name.length() <= 50 && name.find(".") == std::string::npos) info.filename = name + ext; else info.filename = "download" + ext;
        }
        else { info.filename = name; }
    }

    std::string safe_name = info.filename; std::string invalid = "\\/:*?\"<>|";
    for (char c : invalid) safe_name.erase(std::remove(safe_name.begin(), safe_name.end(), c), safe_name.end());
    size_t pos = 0; while ((pos = safe_name.find("%20", pos)) != std::string::npos) { safe_name.replace(pos, 3, " "); pos += 1; }
    if (safe_name.empty()) safe_name = "download.bin";
    info.filename = safe_name; return info;
}

struct WriteData { FILE* fp; int task_id; int thread_index; long long disk_offset; std::vector<char> ram_buffer; };

static size_t write_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    WriteData* wd = (WriteData*)userdata; size_t incoming_bytes = size * nmemb;
    std::shared_ptr<DownloadTask> task = nullptr;
    { std::lock_guard<std::mutex> lock(tasks_mutex); for (auto& t : all_tasks) if (t->id == wd->task_id) { task = t; break; } }
    if (!task || task->is_paused || task->error) return 0;

    long long current = task->thread_states[wd->thread_index].current; long long end = task->thread_states[wd->thread_index].end;
    long long allowed_bytes = end - current + 1; if (allowed_bytes <= 0) return 0;
    size_t bytes_to_write = incoming_bytes;
    if (end != LLONG_MAX && bytes_to_write > static_cast<size_t>(allowed_bytes)) bytes_to_write = static_cast<size_t>(allowed_bytes);

    if (wd->fp) {
        wd->ram_buffer.insert(wd->ram_buffer.end(), (char*)ptr, ((char*)ptr) + bytes_to_write);
        task->thread_states[wd->thread_index].current += bytes_to_write; task->downloaded += bytes_to_write;
        if (wd->ram_buffer.size() >= RAM_BUFFER_SIZE || task->thread_states[wd->thread_index].current > end) {
            std::lock_guard<std::mutex> lock(disk_mutex); _fseeki64(wd->fp, wd->disk_offset, SEEK_SET);
            fwrite(wd->ram_buffer.data(), 1, wd->ram_buffer.size(), wd->fp); wd->disk_offset += wd->ram_buffer.size(); wd->ram_buffer.clear();
        }
    }
    return bytes_to_write;
}

void download_worker(int task_id, int thread_idx, FILE* shared_fp) {
    std::string url; long long start, end, current;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex); std::shared_ptr<DownloadTask> task = nullptr;
        for (auto& t : all_tasks) if (t->id == task_id) task = t;
        if (!task || task->is_paused || task->error) return;
        url = task->url; start = task->thread_states[thread_idx].start; end = task->thread_states[thread_idx].end; current = task->thread_states[thread_idx].current;
    }
    if (current > end) return; CURL* curl = curl_easy_init();
    if (curl && shared_fp) {
        setup_curl(curl); curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (end != LLONG_MAX) { std::string range = std::to_string(current) + "-" + std::to_string(end); curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str()); }
        else { std::string range = std::to_string(current) + "-"; curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str()); }

        WriteData wd = { shared_fp, task_id, thread_idx, current, std::vector<char>() }; wd.ram_buffer.reserve(RAM_BUFFER_SIZE + 1024);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback); curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wd);

        CURLcode res = curl_easy_perform(curl);
        if (!wd.ram_buffer.empty()) { std::lock_guard<std::mutex> lock(disk_mutex); _fseeki64(wd.fp, wd.disk_offset, SEEK_SET); fwrite(wd.ram_buffer.data(), 1, wd.ram_buffer.size(), wd.fp); wd.ram_buffer.clear(); }

        // FIX: Verify if the download TRULY completed or if the Wi-Fi just disconnected and severed the stream
        bool thread_error = false;
        if (res != CURLE_OK) thread_error = true;

        {
            std::lock_guard<std::mutex> lock(tasks_mutex); std::shared_ptr<DownloadTask> task = nullptr;
            for (auto& t : all_tasks) if (t->id == task_id) task = t;
            if (task && !task->is_paused) {
                if (!thread_error) {
                    long long final_current = task->thread_states[thread_idx].current;
                    long long expected_end = task->thread_states[thread_idx].end;
                    // If libcurl finished, but we didn't receive all the bytes, it was a Wi-Fi drop!
                    if (expected_end != LLONG_MAX && final_current <= expected_end) thread_error = true;
                }
                if (thread_error) { task->error = true; task->is_paused = true; } // Safety pause
            }
        }
        curl_easy_cleanup(curl);
    }
}

void StartTaskLogic(int task_id) {
    std::shared_ptr<DownloadTask> task = nullptr;
    { std::lock_guard<std::mutex> lock(tasks_mutex); for (auto& t : all_tasks) if (t->id == task_id) task = t; }
    if (!task) return;
    bool expected = false; if (!task->is_running.compare_exchange_strong(expected, true)) return;
    task->is_paused = false; task->error = false; bool needs_allocation = false;
    { std::lock_guard<std::mutex> lock(tasks_mutex); needs_allocation = task->thread_states.empty(); if (needs_allocation) task->status_text = "Allocating Disk..."; }

    if (needs_allocation) {
        FILE* fp = fopen(task->filename.c_str(), "wb");
        if (fp) { if (task->size > 0) { _fseeki64(fp, task->size - 1, SEEK_SET); fputc('\0', fp); } fclose(fp); }
        else { std::lock_guard<std::mutex> lock(tasks_mutex); task->status_text = "Disk Error"; task->is_running = false; return; }

        std::lock_guard<std::mutex> lock(tasks_mutex);
        if (task->size > 0) {
            long long chunk = task->size / task->active_threads;
            for (int i = 0; i < task->active_threads; i++) {
                long long start = i * chunk; long long end = (i == task->active_threads - 1) ? task->size - 1 : (start + chunk - 1);
                task->thread_states.push_back({ start, end, start });
            }
        }
        else { task->active_threads = 1; task->thread_states.push_back({ 0, LLONG_MAX, 0 }); }
    }

    { std::lock_guard<std::mutex> lock(tasks_mutex); task->active_threads = task->thread_states.size(); task->status_text = "Downloading"; task->workers.clear(); }
    FILE* shared_fp = fopen(task->filename.c_str(), "rb+");
    if (!shared_fp) { std::lock_guard<std::mutex> lock(tasks_mutex); task->status_text = "File Lock Error"; task->is_running = false; return; }

    for (int i = 0; i < task->active_threads; i++) { task->workers.push_back(std::thread(download_worker, task->id, i, shared_fp)); }
    for (auto& t : task->workers) if (t.joinable()) t.join();
    fclose(shared_fp);

    {
        std::lock_guard<std::mutex> lock(tasks_mutex); task->is_running = false;
        if (task->error) {
            task->status_text = "Network Error - Paused";
            if (task->size > 0) {
                std::ofstream out(task->filename + ".idm");
                if (out.is_open()) {
                    out << task->url << "\n" << task->size << "\n" << task->downloaded << "\n" << task->active_threads << "\n";
                    for (const auto& ts : task->thread_states) out << ts.start << " " << ts.end << " " << ts.current << "\n";
                }
            }
        }
        else if (!task->is_paused) {
            task->is_completed = true; task->status_text = "Complete";
            if (task->size <= 0) task->size = task->downloaded;
            if (task->size > 0) task->downloaded = task->size;
            std::string state_file = task->filename + ".idm"; std::remove(state_file.c_str());
        }
        else {
            task->status_text = "Paused";
            if (task->size > 0) {
                std::ofstream out(task->filename + ".idm");
                if (out.is_open()) {
                    out << task->url << "\n" << task->size << "\n" << task->downloaded << "\n" << task->active_threads << "\n";
                    for (const auto& ts : task->thread_states) out << ts.start << " " << ts.end << " " << ts.current << "\n";
                }
            }
        }
    }
    SaveHistory(); // Triggers the hard drive to remember the changes
}

// ==========================================
//    POPUP WINDOW CLASS
// ==========================================

class DownloadProgressWindow : public wxFrame {
public:
    DownloadProgressWindow(wxWindow* parent, int task_id);
    void OnTimer(wxTimerEvent& event); void OnPause(wxCommandEvent& event); void OnHide(wxCommandEvent& event); void OnCancelDownload(wxCommandEvent& event); void ApplyTheme();

private:
    int task_id; wxPanel* panel; wxGauge* progressGauge;
    wxStaticText* lblUrl, * lblPath, * lblStatus, * lblSpeed, * lblDownloaded, * lblTimeLeft, * lblTotalSize;
    wxStaticText* st1, * st2, * st3, * st4, * st5;
    wxButton* btnPause, * btnCancelDl, * btnHide; wxTimer* timer;
};

DownloadProgressWindow::DownloadProgressWindow(wxWindow* parent, int tid)
    : wxFrame(parent, wxID_ANY, "Downloading...", wxDefaultPosition, wxSize(550, 350), wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT), task_id(tid)
{
    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL); panel = new wxPanel(this); wxBoxSizer* panelSizer = new wxBoxSizer(wxVERTICAL);
    std::shared_ptr<DownloadTask> task = nullptr;
    { std::lock_guard<std::mutex> lock(tasks_mutex); for (auto& t : all_tasks) if (t->id == task_id) task = t; }

    wxFont fontBold(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD); wxFont fontNormal(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    std::string display_url = task ? task->url : "Unknown"; if (display_url.length() > 65) display_url = display_url.substr(0, 65) + "...";
    lblUrl = new wxStaticText(panel, wxID_ANY, "URL: " + display_url); lblUrl->SetFont(fontNormal); panelSizer->Add(lblUrl, 0, wxALL | wxEXPAND, 8);
    std::string display_path = task ? task->filename : "..."; if (display_path.length() > 65) display_path = display_path.substr(0, 65) + "...";
    lblPath = new wxStaticText(panel, wxID_ANY, "Saving to: " + display_path); lblPath->SetFont(fontNormal); panelSizer->Add(lblPath, 0, wxALL | wxEXPAND, 8);

    wxBoxSizer* statusSizer = new wxBoxSizer(wxHORIZONTAL); st1 = new wxStaticText(panel, wxID_ANY, "Status: "); st1->SetFont(fontNormal); statusSizer->Add(st1, 0, wxALIGN_CENTER_VERTICAL);
    lblStatus = new wxStaticText(panel, wxID_ANY, "Initializing..."); lblStatus->SetFont(fontBold); statusSizer->Add(lblStatus, 1, wxALIGN_CENTER_VERTICAL); panelSizer->Add(statusSizer, 0, wxALL | wxEXPAND, 8);

    wxFlexGridSizer* grid = new wxFlexGridSizer(2, 2, 12, 12);
    st2 = new wxStaticText(panel, wxID_ANY, "File Size:"); lblTotalSize = new wxStaticText(panel, wxID_ANY, task ? format_bytes(task->size) : "Calculating..."); grid->Add(st2); grid->Add(lblTotalSize);
    st3 = new wxStaticText(panel, wxID_ANY, "Downloaded:"); lblDownloaded = new wxStaticText(panel, wxID_ANY, "0 B"); grid->Add(st3); grid->Add(lblDownloaded);
    st4 = new wxStaticText(panel, wxID_ANY, "Transfer Rate:"); lblSpeed = new wxStaticText(panel, wxID_ANY, "0 KB/sec"); grid->Add(st4); grid->Add(lblSpeed);
    st5 = new wxStaticText(panel, wxID_ANY, "Time Left:"); lblTimeLeft = new wxStaticText(panel, wxID_ANY, "Calculating..."); grid->Add(st5); grid->Add(lblTimeLeft);
    panelSizer->Add(grid, 0, wxALL | wxEXPAND, 15);

    progressGauge = new wxGauge(panel, wxID_ANY, 100); panelSizer->Add(progressGauge, 0, wxALL | wxEXPAND, 15);
    wxBoxSizer* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    btnPause = new wxButton(panel, 1001, "Pause"); btnHide = new wxButton(panel, 1002, "Hide"); btnCancelDl = new wxButton(panel, 1003, "Cancel");
    btnSizer->Add(btnPause, 0, wxRIGHT, 10); btnSizer->Add(btnHide, 0, wxRIGHT, 10); btnSizer->Add(btnCancelDl, 0); panelSizer->Add(btnSizer, 0, wxALIGN_CENTER | wxALL, 10);

    panel->SetSizer(panelSizer); mainSizer->Add(panel, 1, wxEXPAND); SetSizer(mainSizer);
    ApplyTheme();

    Bind(wxEVT_BUTTON, &DownloadProgressWindow::OnPause, this, 1001); Bind(wxEVT_BUTTON, &DownloadProgressWindow::OnHide, this, 1002); Bind(wxEVT_BUTTON, &DownloadProgressWindow::OnCancelDownload, this, 1003);
    timer = new wxTimer(this, 999); Bind(wxEVT_TIMER, &DownloadProgressWindow::OnTimer, this, 999); timer->Start(500);
}

void DownloadProgressWindow::ApplyTheme() {
    if (g_is_dark_mode) {
        panel->SetBackgroundColour(wxColor(40, 40, 40)); wxColor textDark(240, 240, 240);
        lblUrl->SetForegroundColour(textDark); lblPath->SetForegroundColour(textDark); st1->SetForegroundColour(textDark); lblStatus->SetForegroundColour(wxColor(100, 200, 255));
        st2->SetForegroundColour(textDark); lblTotalSize->SetForegroundColour(textDark); st3->SetForegroundColour(textDark); lblDownloaded->SetForegroundColour(textDark);
        st4->SetForegroundColour(textDark); lblSpeed->SetForegroundColour(textDark); st5->SetForegroundColour(textDark); lblTimeLeft->SetForegroundColour(textDark);
    }
    else {
        panel->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)); wxColor textLight(10, 10, 10);
        lblUrl->SetForegroundColour(textLight); lblPath->SetForegroundColour(textLight); st1->SetForegroundColour(textLight); lblStatus->SetForegroundColour(wxColor(0, 100, 200));
        st2->SetForegroundColour(textLight); lblTotalSize->SetForegroundColour(textLight); st3->SetForegroundColour(textLight); lblDownloaded->SetForegroundColour(textLight);
        st4->SetForegroundColour(textLight); lblSpeed->SetForegroundColour(textLight); st5->SetForegroundColour(textLight); lblTimeLeft->SetForegroundColour(textLight);
    }
    panel->Refresh();
}

void DownloadProgressWindow::OnTimer(wxTimerEvent& event) {
    std::shared_ptr<DownloadTask> task = nullptr;
    { std::lock_guard<std::mutex> lock(tasks_mutex); for (auto& t : all_tasks) if (t->id == task_id) task = t; }
    if (!task) { timer->Stop(); Destroy(); return; }

    lblStatus->SetLabel(task->status_text); lblSpeed->SetLabel(format_bytes((long long)task->current_speed) + "/sec");
    if (task->size > 0) {
        lblTotalSize->SetLabel(format_bytes(task->size)); lblDownloaded->SetLabel(format_bytes(task->downloaded) + " (" + std::to_string((int)((double)task->downloaded / task->size * 100)) + "%)"); progressGauge->SetValue((int)((double)task->downloaded / task->size * 100));
        if (task->current_speed > 0) { long long remaining_bytes = task->size - task->downloaded; long long total_seconds = remaining_bytes / (long long)task->current_speed; lblTimeLeft->SetLabel(format_time(total_seconds)); }
    }
    else { lblTotalSize->SetLabel("Unknown"); lblDownloaded->SetLabel(format_bytes(task->downloaded)); lblTimeLeft->SetLabel("Unknown"); }
    if (task->is_completed) { lblStatus->SetLabel("Download Complete"); btnPause->Disable(); btnCancelDl->Disable(); progressGauge->SetValue(100); timer->Stop(); }
}

void DownloadProgressWindow::OnPause(wxCommandEvent& event) { { std::lock_guard<std::mutex> lock(tasks_mutex); for (auto& t : all_tasks) if (t->id == task_id) t->is_paused = true; } timer->Stop(); Destroy(); }
void DownloadProgressWindow::OnHide(wxCommandEvent& event) { timer->Stop(); Destroy(); }
void DownloadProgressWindow::OnCancelDownload(wxCommandEvent& event) {
    { std::lock_guard<std::mutex> lock(tasks_mutex); for (auto& t : all_tasks) if (t->id == task_id) t->is_paused = true; } int id = task_id;
    std::thread([id]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        {
            std::lock_guard<std::mutex> lock(tasks_mutex);
            auto it = std::remove_if(all_tasks.begin(), all_tasks.end(), [id](std::shared_ptr<DownloadTask> t) {
                if (t->id == id) { if (!t->filename.empty()) { std::remove(t->filename.c_str()); std::string state_file = t->filename + ".idm"; std::remove(state_file.c_str()); } return true; } return false;
                }); all_tasks.erase(it, all_tasks.end());
        } SaveHistory();
        }).detach(); timer->Stop(); Destroy();
}

// ==========================================
//             MAIN DASHBOARD UI
// ==========================================

class IDMApp : public wxApp {
public:
    virtual bool OnInit(); virtual int OnExit();
};

class MainFrame : public wxFrame {
public:
    MainFrame(const wxString& title); void ShowAddDialog(const std::string& prefill_url);

private:
    void OnAdd(wxCommandEvent& event); void OnTimer(wxTimerEvent& event); void CheckClipboard(); void ApplyTheme(); void OnToggleTheme(wxCommandEvent& event);
    std::vector<long> GetSelectedRows(); void UpdateToolbarState(); void OnItemDoubleClicked(wxListEvent& event); void OnItemSelected(wxListEvent& event); void OnItemDeselected(wxListEvent& event);
    void OnRightClick(wxListEvent& event); void OnContextResume(wxCommandEvent& event); void OnContextPause(wxCommandEvent& event); void OnContextDelete(wxCommandEvent& event); void OnDeleteAll(wxCommandEvent& event); void OnContextOpenFolder(wxCommandEvent& event); void OnContextProperties(wxCommandEvent& event); void OnContextRefreshLink(wxCommandEvent& event);
    void ExecuteDeletion(const std::vector<int>& ids_to_delete, bool delete_from_disk);

    wxPanel* topHeaderPanel; wxStaticText* headerTitle; wxStaticBitmap* headerLogo;
    wxPanel* customToolbarPanel; wxButton* btnAdd, * btnRes, * btnStop, * btnDel, * btnDelAll, * btnTheme;
    wxListCtrl* listView; wxTimer* updateTimer; std::string last_clipboard_text = "";

    bool m_is_dialog_open = false;
    std::deque<std::string> m_pending_urls;
    std::deque<std::string> m_processed_urls;

    enum { ID_ADD = 101, ID_TIMER = 102, ID_CTX_RESUME = 201, ID_CTX_PAUSE = 202, ID_CTX_DELETE = 203, ID_CTX_OPEN_FOLDER = 204, ID_CTX_PROPS = 205, ID_CTX_REFRESH = 206, ID_DELETE_ALL = 207, ID_THEME_TOGGLE = 301 };
};

wxIMPLEMENT_APP(IDMApp);

bool IDMApp::OnInit() {
    g_InstanceMutex = CreateMutexA(NULL, TRUE, "Global\\FDX_Ultimate_Master_Mutex_V5");

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (argc > 1) {
            std::string passed_url = std::string(argv[1].mb_str());
            if (!passed_url.empty() && passed_url.front() == '"') passed_url.erase(0, 1);
            if (!passed_url.empty() && passed_url.back() == '"') passed_url.pop_back();

            int retries = 10;
            while (retries-- > 0) {
                std::ofstream out(GetQueuePath(), std::ios::app);
                if (out.is_open()) { out << passed_url << "\n"; out.close(); break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        CloseHandle(g_InstanceMutex);
        return false;
    }

    wxImage::AddHandler(new wxPNGHandler()); curl_global_init(CURL_GLOBAL_ALL);
    MainFrame* frame = new MainFrame("Fad Downloader-X"); frame->Show(true);

    if (argc > 1) {
        std::string passed_url = std::string(argv[1].mb_str());
        if (!passed_url.empty() && passed_url.front() == '"') passed_url.erase(0, 1);
        if (!passed_url.empty() && passed_url.back() == '"') passed_url.pop_back();
        std::ofstream out(GetQueuePath(), std::ios::app); out << passed_url << "\n"; out.close();
    }
    return true;
}

int IDMApp::OnExit() { if (g_InstanceMutex) { ReleaseMutex(g_InstanceMutex); CloseHandle(g_InstanceMutex); } return wxApp::OnExit(); }

MainFrame::MainFrame(const wxString& title) : wxFrame(NULL, wxID_ANY, title, wxDefaultPosition, wxSize(1050, 650)) {
    topHeaderPanel = new wxPanel(this, wxID_ANY); wxBoxSizer* topSizer = new wxBoxSizer(wxHORIZONTAL);
    wxBitmap logoBmp; if (wxFileExists("logo.png")) logoBmp = wxBitmap("logo.png", wxBITMAP_TYPE_PNG); else logoBmp = wxArtProvider::GetBitmap(wxART_HARDDISK, wxART_OTHER, wxSize(48, 48));
    wxIcon appIcon; appIcon.CopyFromBitmap(logoBmp); SetIcon(appIcon);
    headerLogo = new wxStaticBitmap(topHeaderPanel, wxID_ANY, logoBmp); headerTitle = new wxStaticText(topHeaderPanel, wxID_ANY, "  Fad Downloader-X"); headerTitle->SetFont(wxFont(20, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
    topSizer->Add(headerLogo, 0, wxALL | wxALIGN_CENTER_VERTICAL, 15); topSizer->Add(headerTitle, 0, wxALL | wxALIGN_CENTER_VERTICAL, 15); topHeaderPanel->SetSizer(topSizer);

    customToolbarPanel = new wxPanel(this, wxID_ANY); wxBoxSizer* tbSizer = new wxBoxSizer(wxHORIZONTAL);
    btnAdd = new wxButton(customToolbarPanel, ID_ADD, " Add URL ", wxDefaultPosition, wxDefaultSize, wxBORDER_NONE); btnAdd->SetBitmap(wxArtProvider::GetBitmap(wxART_NEW, wxART_TOOLBAR));
    btnRes = new wxButton(customToolbarPanel, ID_CTX_RESUME, " Resume ", wxDefaultPosition, wxDefaultSize, wxBORDER_NONE); btnRes->SetBitmap(wxArtProvider::GetBitmap(wxART_GO_FORWARD, wxART_TOOLBAR));
    btnStop = new wxButton(customToolbarPanel, ID_CTX_PAUSE, " Stop ", wxDefaultPosition, wxDefaultSize, wxBORDER_NONE); btnStop->SetBitmap(wxArtProvider::GetBitmap(wxART_CROSS_MARK, wxART_TOOLBAR));
    btnDel = new wxButton(customToolbarPanel, ID_CTX_DELETE, " Delete ", wxDefaultPosition, wxDefaultSize, wxBORDER_NONE); btnDel->SetBitmap(wxArtProvider::GetBitmap(wxART_DELETE, wxART_TOOLBAR));
    btnDelAll = new wxButton(customToolbarPanel, ID_DELETE_ALL, " Delete All ", wxDefaultPosition, wxDefaultSize, wxBORDER_NONE); btnDelAll->SetBitmap(wxArtProvider::GetBitmap(wxART_MINUS, wxART_TOOLBAR));
    btnTheme = new wxButton(customToolbarPanel, ID_THEME_TOGGLE, " Toggle Theme ", wxDefaultPosition, wxDefaultSize, wxBORDER_NONE); btnTheme->SetBitmap(wxArtProvider::GetBitmap(wxART_HELP_SETTINGS, wxART_TOOLBAR));
    tbSizer->Add(btnAdd, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5); tbSizer->Add(btnRes, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5); tbSizer->Add(btnStop, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5); tbSizer->Add(btnDel, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5); tbSizer->Add(btnDelAll, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5); tbSizer->AddStretchSpacer(1); tbSizer->Add(btnTheme, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5); customToolbarPanel->SetSizer(tbSizer);

    listView = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);
    listView->InsertColumn(0, "ID", wxLIST_FORMAT_LEFT, 50); listView->InsertColumn(1, "Filename", wxLIST_FORMAT_LEFT, 320); listView->InsertColumn(2, "Size", wxLIST_FORMAT_RIGHT, 100); listView->InsertColumn(3, "Status", wxLIST_FORMAT_CENTER, 150); listView->InsertColumn(4, "Progress", wxLIST_FORMAT_RIGHT, 100); listView->InsertColumn(5, "Transfer Rate", wxLIST_FORMAT_RIGHT, 120);

    wxBoxSizer* mainSizer = new wxBoxSizer(wxVERTICAL); mainSizer->Add(topHeaderPanel, 0, wxEXPAND); mainSizer->Add(customToolbarPanel, 0, wxEXPAND); mainSizer->Add(listView, 1, wxEXPAND | wxALL, 0); SetSizer(mainSizer);
    CreateStatusBar(); ApplyTheme(); UpdateToolbarState();

    LoadHistory(listView); // Restores everything you downloaded before!

    updateTimer = new wxTimer(this, ID_TIMER); updateTimer->Start(500);

    Bind(wxEVT_BUTTON, &MainFrame::OnToggleTheme, this, ID_THEME_TOGGLE); Bind(wxEVT_BUTTON, &MainFrame::OnAdd, this, ID_ADD); Bind(wxEVT_BUTTON, &MainFrame::OnContextResume, this, ID_CTX_RESUME); Bind(wxEVT_BUTTON, &MainFrame::OnContextPause, this, ID_CTX_PAUSE); Bind(wxEVT_BUTTON, &MainFrame::OnContextDelete, this, ID_CTX_DELETE); Bind(wxEVT_BUTTON, &MainFrame::OnDeleteAll, this, ID_DELETE_ALL); Bind(wxEVT_TIMER, &MainFrame::OnTimer, this, ID_TIMER);
    listView->Bind(wxEVT_LIST_ITEM_ACTIVATED, &MainFrame::OnItemDoubleClicked, this); listView->Bind(wxEVT_LIST_ITEM_SELECTED, &MainFrame::OnItemSelected, this); listView->Bind(wxEVT_LIST_ITEM_DESELECTED, &MainFrame::OnItemDeselected, this); listView->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, &MainFrame::OnRightClick, this);
    Bind(wxEVT_MENU, &MainFrame::OnContextResume, this, ID_CTX_RESUME); Bind(wxEVT_MENU, &MainFrame::OnContextPause, this, ID_CTX_PAUSE); Bind(wxEVT_MENU, &MainFrame::OnContextDelete, this, ID_CTX_DELETE); Bind(wxEVT_MENU, &MainFrame::OnContextOpenFolder, this, ID_CTX_OPEN_FOLDER); Bind(wxEVT_MENU, &MainFrame::OnContextProperties, this, ID_CTX_PROPS); Bind(wxEVT_MENU, &MainFrame::OnContextRefreshLink, this, ID_CTX_REFRESH);
}

void MainFrame::OnToggleTheme(wxCommandEvent& event) { g_is_dark_mode = !g_is_dark_mode; ApplyTheme(); }
void MainFrame::ApplyTheme() {
    if (g_is_dark_mode) {
        wxColor darkBg(35, 35, 35); wxColor tbBg(45, 45, 45); wxColor darkListBg(30, 30, 30); wxColor darkText(240, 240, 240);
        topHeaderPanel->SetBackgroundColour(darkBg); headerTitle->SetForegroundColour(darkText);
        customToolbarPanel->SetBackgroundColour(tbBg); btnAdd->SetBackgroundColour(tbBg); btnAdd->SetForegroundColour(darkText); btnRes->SetBackgroundColour(tbBg); btnRes->SetForegroundColour(darkText); btnStop->SetBackgroundColour(tbBg); btnStop->SetForegroundColour(darkText); btnDel->SetBackgroundColour(tbBg); btnDel->SetForegroundColour(darkText); btnDelAll->SetBackgroundColour(tbBg); btnDelAll->SetForegroundColour(darkText); btnTheme->SetBackgroundColour(tbBg); btnTheme->SetForegroundColour(darkText);
        listView->SetBackgroundColour(darkListBg); listView->SetForegroundColour(darkText); listView->SetTextColour(darkText); listView->EnableAlternateRowColours(true); SetBackgroundColour(darkBg);
    }
    else {
        wxColor lightBg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW); wxColor tbBg(240, 240, 240); wxColor lightText = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        topHeaderPanel->SetBackgroundColour(wxColor(255, 255, 255)); headerTitle->SetForegroundColour(lightText);
        customToolbarPanel->SetBackgroundColour(tbBg); btnAdd->SetBackgroundColour(tbBg); btnAdd->SetForegroundColour(lightText); btnRes->SetBackgroundColour(tbBg); btnRes->SetForegroundColour(lightText); btnStop->SetBackgroundColour(tbBg); btnStop->SetForegroundColour(lightText); btnDel->SetBackgroundColour(tbBg); btnDel->SetForegroundColour(lightText); btnDelAll->SetBackgroundColour(tbBg); btnDelAll->SetForegroundColour(lightText); btnTheme->SetBackgroundColour(tbBg); btnTheme->SetForegroundColour(lightText);
        listView->SetBackgroundColour(lightBg); listView->SetForegroundColour(lightText); listView->SetTextColour(lightText); listView->EnableAlternateRowColours(true); SetBackgroundColour(tbBg);
    } Refresh(); Update();
}

std::vector<long> MainFrame::GetSelectedRows() { std::vector<long> rows; long item = -1; for (;;) { item = listView->GetNextItem(item, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED); if (item == -1) break; rows.push_back(item); } return rows; }
void MainFrame::UpdateToolbarState() {
    auto rows = GetSelectedRows(); if (rows.empty()) { btnRes->Enable(false); btnStop->Enable(false); btnDel->Enable(false); return; }
    btnDel->Enable(true); bool can_resume = false; bool can_pause = false;
    { std::lock_guard<std::mutex> lock(tasks_mutex); for (long row : rows) { int id = std::atoi(listView->GetItemText(row, 0).c_str()); for (auto& t : all_tasks) { if (t->id == id) { if (t->is_paused || (!t->is_running && !t->is_completed)) can_resume = true; if (t->is_running && !t->is_paused) can_pause = true; } } } }
    btnRes->Enable(can_resume); btnStop->Enable(can_pause);
}
void MainFrame::OnItemSelected(wxListEvent& event) { UpdateToolbarState(); } void MainFrame::OnItemDeselected(wxListEvent& event) { UpdateToolbarState(); }
void MainFrame::OnItemDoubleClicked(wxListEvent& event) { long row = event.GetIndex(); if (row == -1) return; int id = std::atoi(listView->GetItemText(row, 0).c_str()); std::shared_ptr<DownloadTask> task = nullptr; { std::lock_guard<std::mutex> lock(tasks_mutex); for (auto& t : all_tasks) if (t->id == id) task = t; } if (task && task->is_completed && !task->filename.empty()) { if (wxFileExists(task->filename)) wxLaunchDefaultApplication(task->filename); } }

void MainFrame::ShowAddDialog(const std::string& prefill_url) {
    wxTextEntryDialog urlDialog(this, "Address:", "Enter URL", prefill_url);
    if (urlDialog.ShowModal() == wxID_OK) {
        std::string url = urlDialog.GetValue().ToStdString(); url.erase(0, url.find_first_not_of(" \t\r\n")); url.erase(url.find_last_not_of(" \t\r\n") + 1);
        wxProgressDialog fetchDialog("Connecting", "Fetching file information from server...\nPlease wait.", 100, this, wxPD_APP_MODAL | wxPD_AUTO_HIDE);
        std::atomic<bool> fetch_done{ false }; FileInfo info; std::thread([&]() { info = GetRobustFileInfo(url); fetch_done = true; }).detach();
        while (!fetch_done) { fetchDialog.Pulse(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); wxYield(); } fetchDialog.Update(100);

        std::string safe_name = info.filename; std::string invalid = "\\/:*?\"<>|";
        for (char c : invalid) safe_name.erase(std::remove(safe_name.begin(), safe_name.end(), c), safe_name.end());
        if (safe_name.empty()) safe_name = "download.bin";
        if (safe_name.length() > 100) safe_name = "video_download.mp4";

        wxFileDialog saveDialog(this, "Save File As", "", safe_name, "All files (*.*)|*.*", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (saveDialog.ShowModal() == wxID_CANCEL) return;

        std::string savePath = saveDialog.GetPath().ToStdString(); std::string shortName = saveDialog.GetFilename().ToStdString();
        int new_id = g_id_counter++; auto task = std::make_shared<DownloadTask>(new_id, url); task->filename = savePath; task->short_name = shortName; task->size = info.size; task->active_threads = (info.supports_resume && info.size > 0) ? NUM_THREADS : 1;
        { std::lock_guard<std::mutex> lock(tasks_mutex); all_tasks.push_back(task); }
        long index = listView->InsertItem(listView->GetItemCount(), std::to_string(new_id)); listView->SetItem(index, 1, task->short_name); listView->SetItem(index, 3, "Pending");
        SaveHistory();
        std::thread(StartTaskLogic, new_id).detach(); DownloadProgressWindow* dlg = new DownloadProgressWindow(this, new_id); dlg->Show();
    }
}

void MainFrame::OnAdd(wxCommandEvent& event) {
    std::string clipUrl = "";
    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_TEXT)) {
            wxTextDataObject data; wxTheClipboard->GetData(data); std::string text = data.GetText().ToStdString();
            if (text.find("http") == 0) clipUrl = text;
        } wxTheClipboard->Close();
    }
    if (!m_is_dialog_open) { m_is_dialog_open = true; ShowAddDialog(clipUrl); m_is_dialog_open = false; }
    else { if (!clipUrl.empty()) m_pending_urls.push_back(clipUrl); }
}

void MainFrame::CheckClipboard() {
    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_TEXT)) {
            wxTextDataObject data;
            if (wxTheClipboard->GetData(data)) {
                std::string text = data.GetText().ToStdString();
                if (!text.empty() && text != last_clipboard_text && IsDownloadableLink(text)) {
                    if (std::find(m_processed_urls.begin(), m_processed_urls.end(), text) == m_processed_urls.end()) {
                        m_pending_urls.push_back(text); m_processed_urls.push_back(text);
                        if (m_processed_urls.size() > 500) m_processed_urls.pop_front();
                    }
                    last_clipboard_text = text;
                }
            }
        } wxTheClipboard->Close();
    }
}

void MainFrame::OnRightClick(wxListEvent& event) {
    long row = event.GetIndex(); listView->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    bool can_resume = false; bool can_pause = false; auto rows = GetSelectedRows();
    { std::lock_guard<std::mutex> lock(tasks_mutex); for (long r : rows) { int id = std::atoi(listView->GetItemText(r, 0).c_str()); for (auto& t : all_tasks) { if (t->id == id) { if (t->is_paused || (!t->is_running && !t->is_completed)) can_resume = true; if (t->is_running && !t->is_paused) can_pause = true; } } } }
    wxMenu menu; menu.Append(ID_CTX_PROPS, "Show Progress Window"); menu.AppendSeparator(); menu.Append(ID_CTX_RESUME, "Resume Download"); menu.Append(ID_CTX_PAUSE, "Stop Download"); menu.Append(ID_CTX_REFRESH, "Refresh Download Address"); menu.AppendSeparator(); menu.Append(ID_CTX_OPEN_FOLDER, "Open Folder"); menu.Append(ID_CTX_DELETE, "Remove"); menu.Enable(ID_CTX_RESUME, can_resume); menu.Enable(ID_CTX_PAUSE, can_pause); PopupMenu(&menu);
}

void MainFrame::OnContextResume(wxCommandEvent& event) { auto rows = GetSelectedRows(); for (long row : rows) { int id = std::atoi(listView->GetItemText(row, 0).c_str()); std::shared_ptr<DownloadTask> task = nullptr; { std::lock_guard<std::mutex> lock(tasks_mutex); for (auto& t : all_tasks) if (t->id == id) task = t; } if (task && !task->is_running && !task->is_completed) std::thread(StartTaskLogic, id).detach(); } UpdateToolbarState(); }
void MainFrame::OnContextPause(wxCommandEvent& event) { auto rows = GetSelectedRows(); { std::lock_guard<std::mutex> lock(tasks_mutex); for (long row : rows) { int id = std::atoi(listView->GetItemText(row, 0).c_str()); for (auto& t : all_tasks) if (t->id == id) t->is_paused = true; } } UpdateToolbarState(); SaveHistory(); }
void MainFrame::OnContextRefreshLink(wxCommandEvent& event) { auto rows = GetSelectedRows(); if (rows.empty()) return; int id = std::atoi(listView->GetItemText(rows[0], 0).c_str()); std::shared_ptr<DownloadTask> task = nullptr; { std::lock_guard<std::mutex> lock(tasks_mutex); for (auto& t : all_tasks) if (t->id == id) task = t; } if (task) { wxTextEntryDialog urlDialog(this, "Enter new download address for resume:", "Refresh Address"); urlDialog.SetValue(task->url); if (urlDialog.ShowModal() == wxID_OK) { std::lock_guard<std::mutex> lock(tasks_mutex); task->url = urlDialog.GetValue().ToStdString(); task->error = false; task->status_text = "Address Updated"; task->is_paused = true; } } SaveHistory(); }
void MainFrame::ExecuteDeletion(const std::vector<int>& ids_to_delete, bool delete_from_disk) { { std::lock_guard<std::mutex> lock(tasks_mutex); for (int id : ids_to_delete) { for (auto& t : all_tasks) if (t->id == id) t->is_paused = true; } } for (int id : ids_to_delete) { for (int i = 0; i < listView->GetItemCount(); i++) { if (std::atoi(listView->GetItemText(i, 0).c_str()) == id) { listView->DeleteItem(i); break; } } } UpdateToolbarState(); std::thread([this, ids_to_delete, delete_from_disk]() { std::this_thread::sleep_for(std::chrono::milliseconds(1500)); { std::lock_guard<std::mutex> lock(tasks_mutex); auto it = std::remove_if(all_tasks.begin(), all_tasks.end(), [&](std::shared_ptr<DownloadTask> t) { if (std::find(ids_to_delete.begin(), ids_to_delete.end(), t->id) != ids_to_delete.end()) { if (!t->filename.empty()) { std::string state_file = t->filename + ".idm"; std::remove(state_file.c_str()); if (delete_from_disk) std::remove(t->filename.c_str()); } return true; } return false; }); all_tasks.erase(it, all_tasks.end()); } SaveHistory(); }).detach(); }
void MainFrame::OnContextDelete(wxCommandEvent& event) { auto rows = GetSelectedRows(); if (rows.empty()) return; wxMessageDialog dlg(this, "Delete the downloaded file(s) from your hard drive as well?\n\nYes = Remove from List & Disk\nNo = Remove from List Only", "Confirm Deletion", wxYES_NO | wxCANCEL | wxICON_QUESTION); int result = dlg.ShowModal(); if (result == wxID_CANCEL) return; std::vector<int> ids; for (long row : rows) ids.push_back(std::atoi(listView->GetItemText(row, 0).c_str())); ExecuteDeletion(ids, (result == wxID_YES)); }
void MainFrame::OnDeleteAll(wxCommandEvent& event) { if (listView->GetItemCount() == 0) return; wxMessageDialog dlg(this, "Are you sure you want to remove ALL downloads?\n\nYes = Remove ALL from List & Disk\nNo = Remove ALL from List Only", "Delete All Downloads", wxYES_NO | wxCANCEL | wxICON_WARNING); int result = dlg.ShowModal(); if (result == wxID_CANCEL) return; std::vector<int> ids; for (int i = 0; i < listView->GetItemCount(); i++) ids.push_back(std::atoi(listView->GetItemText(i, 0).c_str())); ExecuteDeletion(ids, (result == wxID_YES)); }
void MainFrame::OnContextProperties(wxCommandEvent& event) { auto rows = GetSelectedRows(); if (rows.empty()) return; int id = std::atoi(listView->GetItemText(rows[0], 0).c_str()); DownloadProgressWindow* dlg = new DownloadProgressWindow(this, id); dlg->Show(); }
void MainFrame::OnContextOpenFolder(wxCommandEvent& event) { auto rows = GetSelectedRows(); if (rows.empty()) return; int id = std::atoi(listView->GetItemText(rows[0], 0).c_str()); std::shared_ptr<DownloadTask> task = nullptr; { std::lock_guard<std::mutex> lock(tasks_mutex); for (auto& t : all_tasks) if (t->id == id) task = t; } if (task && !task->filename.empty()) { std::string cmd = "explorer /select,\"" + task->filename + "\""; system(cmd.c_str()); } else { system("explorer ."); } }

void MainFrame::OnTimer(wxTimerEvent& event) {
    CheckClipboard();

    std::string queuePath = GetQueuePath();
    if (wxFileExists(queuePath)) {
        std::vector<std::string> new_urls;
        std::ifstream in(queuePath);
        if (in.is_open()) {
            std::string line;
            while (std::getline(in, line)) {
                line.erase(0, line.find_first_not_of(" \t\r\n")); line.erase(line.find_last_not_of(" \t\r\n") + 1);
                if (!line.empty() && std::find(m_processed_urls.begin(), m_processed_urls.end(), line) == m_processed_urls.end()) {
                    new_urls.push_back(line); m_processed_urls.push_back(line);
                    if (m_processed_urls.size() > 500) m_processed_urls.pop_front();
                }
            }
            in.close();

            std::ofstream clear_file(queuePath, std::ios::trunc);
            clear_file.close();
            std::remove(queuePath.c_str());

            int added = 0;
            for (const auto& u : new_urls) { if (added < 3) { m_pending_urls.push_back(u); added++; } }
        }
    }

    if (!m_is_dialog_open && !m_pending_urls.empty()) {
        std::string next_url = m_pending_urls.front(); m_pending_urls.pop_front();
        m_is_dialog_open = true;
        this->CallAfter([this, next_url]() { Restore(); Raise(); ShowAddDialog(next_url); m_is_dialog_open = false; });
    }

    bool toolbar_needs_update = false; std::vector<int> to_remove_ui;
    for (int i = 0; i < listView->GetItemCount(); i++) {
        int id = std::atoi(listView->GetItemText(i, 0).c_str()); bool found = false;
        { std::lock_guard<std::mutex> lock(tasks_mutex); for (auto& t : all_tasks) if (t->id == id) { found = true; break; } }
        if (!found) to_remove_ui.push_back(id);
    }
    for (int id : to_remove_ui) { for (int i = 0; i < listView->GetItemCount(); i++) { if (std::atoi(listView->GetItemText(i, 0).c_str()) == id) { listView->DeleteItem(i); toolbar_needs_update = true; break; } } }

    {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        for (int i = 0; i < listView->GetItemCount(); i++) {
            int id = std::atoi(listView->GetItemText(i, 0).c_str()); std::shared_ptr<DownloadTask> task = nullptr;
            for (auto& t : all_tasks) if (t->id == id) task = t;
            if (task) {
                if (listView->GetItemText(i, 3) != task->status_text) toolbar_needs_update = true;
                listView->SetItem(i, 1, task->short_name); listView->SetItem(i, 3, task->status_text);
                if (task->size > 0) {
                    listView->SetItem(i, 2, format_bytes(task->size)); double pct = (double)task->downloaded / task->size * 100.0; listView->SetItem(i, 4, std::to_string((int)pct) + "%");
                    if (task->is_running && !task->is_completed) {
                        long long diff = task->downloaded - task->last_bytes; double raw_speed = (diff / 1024.0 / 1024.0) * 2.0;
                        task->speed_history.push_back(raw_speed); if (task->speed_history.size() > SPEED_SAMPLES) task->speed_history.pop_front();
                        double avg = std::accumulate(task->speed_history.begin(), task->speed_history.end(), 0.0) / task->speed_history.size(); task->current_speed = avg * 1024 * 1024;
                        std::stringstream ss; ss << std::fixed << std::setprecision(2) << avg << " MB/s"; listView->SetItem(i, 5, ss.str()); task->last_bytes = task->downloaded;
                    }
                    else { listView->SetItem(i, 5, ""); task->current_speed = 0; }
                }
            }
        }
    }
    if (toolbar_needs_update) UpdateToolbarState();
}