#include "hh_scanner.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>

#include <time.h>
#include <tlhelp32.h>

#include "util/suspend.h"
#include "util/time_util.h"
#include "term_util.h"

#include <paramkit.h>

#define PID_FIELD_SIZE 4

using namespace pesieve;

namespace process_util {

    bool is_wow_64(HANDLE process)
    {
        FARPROC procPtr = GetProcAddress(GetModuleHandleA("kernel32"), "IsWow64Process");
        if (!procPtr) {
            //this system does not have a function IsWow64Process
            return false;
        }
        BOOL(WINAPI * is_process_wow64)(IN HANDLE, OUT PBOOL)
            = (BOOL(WINAPI*)(IN HANDLE, OUT PBOOL))procPtr;

        BOOL isCurrWow64 = FALSE;
        if (!is_process_wow64(process, &isCurrWow64)) {
            return false;
        }
        return isCurrWow64 ? true : false;
    }

    bool get_process_info(DWORD processID, bool &isWow64, char* szProcessName, DWORD processNameSize)
    {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, processID);
        if (!hProcess) {
            return false;
        }

        bool is_ok = true;
        if (szProcessName && processNameSize > 0) {
            DWORD cbNeeded = 0;
            HMODULE hMod = nullptr;
            memset(szProcessName, 0, processNameSize);
            if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), &cbNeeded)) {
                GetModuleBaseNameA(hProcess, hMod, szProcessName, processNameSize);
            } else { is_ok = false; }
        }
        isWow64 = is_wow_64(hProcess);
        CloseHandle(hProcess);
        return is_ok;
    }

    size_t suspend_suspicious(std::vector<DWORD> &suspicious_pids)
    {
        size_t done = 0;
        std::vector<DWORD>::iterator itr;
        for (itr = suspicious_pids.begin(); itr != suspicious_pids.end(); ++itr) {
            DWORD pid = *itr;
            if (!suspend_process(pid)) {
                std::cerr << "Could not suspend the process. PID = " << pid << std::endl;
            }
        }
        return done;
    }

    size_t kill_suspicious(std::vector<DWORD> &suspicious_pids)
    {
        size_t killed = 0;
        std::vector<DWORD>::iterator itr;
        for (itr = suspicious_pids.begin(); itr != suspicious_pids.end(); ++itr) {
            DWORD pid = *itr;
            HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (!hProcess) {
                continue;
            }
            if (TerminateProcess(hProcess, 0)) {
                killed++;
            }
            else {
                std::cerr << "Could not terminate the process. PID = " << pid << std::endl;
            }
            CloseHandle(hProcess);
        }
        return killed;
    }

}; // namespace process_util


namespace files_util {

    std::string join_path(const std::string &baseDir, const std::string &subpath)
    {
        std::stringstream stream;
        if (baseDir.length() > 0) {
            stream << baseDir;
            stream << "\\";
        }
        stream << subpath;
        return stream.str();
    }

    std::string make_dir_name(std::string baseDir, time_t timestamp)
    {
        std::stringstream stream;
        if (baseDir.length() > 0) {
            stream << baseDir;
            stream << "\\";
        }
        stream << "scan_";
        stream << timestamp;
        return stream.str();
    }

    bool set_output_dir(t_params &args, const std::string &new_dir)
    {
        const size_t new_len = new_dir.length();
        if (!new_len) return false;

        const char* new_dir_cstr = new_dir.c_str();
        size_t buffer_len = sizeof(args.output_dir) - 1; //leave one char for '\0'
        if (new_len > buffer_len) return false;

        memset(args.output_dir, 0, buffer_len);
        memcpy(args.output_dir, new_dir_cstr, new_len);
        return true;
    }

    bool write_to_file(const std::string &report_path, const std::string &summary_str, const bool append)
    {
        std::ofstream final_report;
        if (append) {
            final_report.open(report_path, std::ios_base::app);
        }
        else {
            final_report.open(report_path);
        }
        if (final_report.is_open()) {
            final_report << summary_str;
            final_report.close();
            return true;
        }
        return false;
    }
}; // namespace files_util 

namespace util {

    bool is_searched_name(const char* processName, std::set<std::string> &names_list)
    {
        std::set<std::string>::iterator itr;
        for (itr = names_list.begin(); itr != names_list.end(); ++itr) {
            const char* searchedName = itr->c_str();
            if (_stricmp(processName, searchedName) == 0) {
                return true;
            }
        }
        return false;
    }

    bool is_searched_pid(long pid, std::set<long> &pids_list)
    {
        std::set<long>::iterator found = pids_list.find(pid);
        if (found != pids_list.end()) {
            return true;
        }
        return false;
    }

    template <typename TYPE_T>
    std::string list_to_str(std::set<TYPE_T> &list)
    {
        std::stringstream stream;

        std::set<TYPE_T>::iterator itr;
        for (itr = list.begin(); itr != list.end(); ) {
            stream << *itr;
            ++itr;
            if (itr != list.end()) {
                stream << ", ";
            }
        }
        return stream.str();
    }

}; //namespace util

//----

HHScanner::HHScanner(t_hh_params &_args)
    : hh_args(_args)
{
    initTime = time(NULL);
    isScannerWow64 = process_util::is_wow_64(GetCurrentProcess());
}

bool HHScanner::isScannerCompatibile()
{
#ifndef _WIN64
    if (process_util::is_wow_64(GetCurrentProcess())) {
        return false;
    }
#endif
    return true;
}

void HHScanner::initOutDir(time_t scan_time, pesieve::t_params &pesieve_args)
{
    //set unique path
    if (hh_args.unique_dir) {
        this->outDir = files_util::make_dir_name(hh_args.out_dir, scan_time);
        files_util::set_output_dir(pesieve_args, outDir);
    }
    else {
        this->outDir = hh_args.out_dir;
        files_util::set_output_dir(pesieve_args, hh_args.out_dir);
    }
}

void HHScanner::printScanRoundStats(size_t found, size_t ignored_count)
{
    if (!found && hh_args.names_list.size() > 0) {
        if (!hh_args.quiet) {
            std::cout << "[WARNING] No process from the list: {" << util::list_to_str(hh_args.names_list) << "} was found!" << std::endl;
        }
    }
    if (!found && hh_args.pids_list.size() > 0) {
        if (!hh_args.quiet) {
            std::cout << "[WARNING] No process from the list: {" << util::list_to_str(hh_args.pids_list) << "} was found!" << std::endl;
        }
    }
    if (ignored_count > 0) {
        if (!hh_args.quiet) {
            std::string info1 = (ignored_count > 1) ? "processes" : "process";
            std::string info2 = (ignored_count > 1) ? "were" : "was";
            std::cout << "[INFO] " << std::dec << ignored_count << " " << info1 << " from the list : {" << util::list_to_str(hh_args.ignored_names_list) << "} " << info2 << " ignored!" << std::endl;
        }
    }
}

size_t HHScanner::scanProcesses(HHScanReport &my_report)
{
    size_t count = 0;
    size_t scanned_count = 0;
    size_t ignored_count = 0;

    HANDLE hProcessSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnapShot == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        std::cerr << "[-] Could not create modules snapshot. Error: " << std::dec << err << std::endl;
        return 0;
    }

    PROCESSENTRY32 pe32 = { 0 };
    pe32.dwSize = sizeof(PROCESSENTRY32);

    //check all modules in the process, including the main module:
    if (!Process32First(hProcessSnapShot, &pe32)) {
        CloseHandle(hProcessSnapShot);
        std::cerr << "[-] Could not enumerate processes. Error: " << GetLastError() << std::endl;
        return 0;
    }
    do {
        // scan callback
        const t_single_scan_status stat = scanNextProcess(pe32.th32ProcessID, pe32.szExeFile, my_report);
        if (stat == SSCAN_IGNORED) ignored_count++;
        if (stat == SSCAN_SUCCESS) scanned_count++;
        count++;

    } while (Process32Next(hProcessSnapShot, &pe32));

    //close the handles
    CloseHandle(hProcessSnapShot);

    printScanRoundStats(scanned_count, ignored_count);
    return count;
}

void HHScanner::printSingleReport(pesieve::t_report& report)
{
    if (hh_args.quiet) return;

    if (report.errors == pesieve::ERROR_SCAN_FAILURE) {
        WORD old_color = set_color(MAKE_COLOR(SILVER, DARK_RED));
        if (report.errors == pesieve::ERROR_SCAN_FAILURE) {
            std::cout << "[!] Could not access: " << std::dec << report.pid;
        }
        set_color(old_color);
        std::cout << std::endl;
        return;
    }
#ifndef _WIN64
    if (report.is_64bit) {
        WORD old_color = set_color(MAKE_COLOR(SILVER, DARK_MAGENTA));
        std::cout << "[!] Partial scan: " << std::dec << report.pid << " : " << (report.is_64bit ? 64 : 32) << "b";
        set_color(old_color);
        std::cout << std::endl;
    }
#endif
    if (report.suspicious) {
        int color = YELLOW;
        if (report.replaced || report.implanted) {
            color = RED;
        }
        if (report.is_managed) {
            color = MAKE_COLOR(color, DARK_BLUE);
        }
        WORD old_color = set_color(color);
        std::cout << ">> Detected: " << std::dec << report.pid;
        if (report.is_managed) {
            std::cout << " [.NET]";
        }
        set_color(old_color);
        std::cout << std::endl;
    }
}

t_single_scan_status HHScanner::scanNextProcess(DWORD pid, char* exe_file, HHScanReport &my_report)
{
    bool found = false;

    bool is_process_wow64 = false;
    process_util::get_process_info(pid, is_process_wow64, nullptr, 0);

    const bool check_time = (hh_args.ptimes != TIME_UNDEFINED) ? true : false;
#ifdef _DEBUG
    if (check_time) {
        std::cout << "Init Time: " << std::hex << this->initTime << std::endl;
    }
#endif
    // filter by the time
    time_t time_diff = 0;
    if (check_time) { // if the parameter was set
        const time_t process_time = util::process_start_time(pid);
        if (process_time == INVALID_TIME) return SSCAN_ERROR0; //skip process if cannot retrieve the time

        // if HH was started after the process
        if (this->initTime > process_time) {
            time_diff = this->initTime - process_time;
            if (time_diff > hh_args.ptimes) return SSCAN_NOT_MATCH; // skip process created before the supplied time
        }
    }
    //filter by the names/PIDs
    if (hh_args.names_list.size() || hh_args.pids_list.size()) {
        if (!util::is_searched_name(exe_file, hh_args.names_list) && !util::is_searched_pid(pid, hh_args.pids_list)) {
            //it is not the searched process, so skip it
            return SSCAN_NOT_MATCH;
        }
        found = true;
    }
    if (!found && hh_args.ignored_names_list.size()) {
        if (util::is_searched_name(exe_file, hh_args.ignored_names_list)) {
            return SSCAN_IGNORED;
        }
    }
    if (!hh_args.quiet) {
        std::cout << ">> Scanning PID: " << std::setw(PID_FIELD_SIZE) << std::dec << pid;
        std::cout << " : " << exe_file;

        if (is_process_wow64) {
            std::cout << " : 32b";
        }
        if (check_time) {
            std::cout << " : " << time_diff << "s";
        }
        std::cout << std::endl;
    }
    //perform the scan:
    pesieve::t_params &pesieve_args = this->hh_args.pesieve_args;
    pesieve_args.pid = pid;

    pesieve::t_report report = PESieve_scan(pesieve_args);
    my_report.appendReport(report, exe_file);

    printSingleReport(report);
    if (report.scanned > 0) {
        return SSCAN_SUCCESS;
    }
    return SSCAN_ERROR1;
}

HHScanReport* HHScanner::scan()
{
    const time_t scan_start = time(NULL); //start time of the current scan
    pesieve::t_params &pesieve_args = this->hh_args.pesieve_args;
    initOutDir(scan_start, pesieve_args);

    HHScanReport *my_report = new HHScanReport(GetTickCount(), scan_start);
    scanProcesses(*my_report);

    my_report->setEndTick(GetTickCount(), time(NULL));
    return my_report;
}

void HHScanner::summarizeScan(HHScanReport *hh_report)
{
    if (!hh_report) return;
    std::string summary_str;

    if (!this->hh_args.json_output) {
        summary_str = hh_report->toString();
        std::cout << summary_str;
    }
    else {
        summary_str = hh_report->toJSON(this->hh_args);
        std::cout << summary_str;
    }

    if (hh_args.pesieve_args.out_filter != OUT_NO_DIR) {
        //file the same report into the directory with dumps:
        if (hh_report->suspicious.size()) {
            std::string report_path = files_util::join_path(this->outDir, "summary.json");
            files_util::write_to_file(report_path, hh_report->toJSON(this->hh_args), false);
        }
    }
    if (hh_args.log) {
        files_util::write_to_file("hollows_hunter.log", summary_str, true);
    }
    if (hh_args.suspend_suspicious) {
        process_util::suspend_suspicious(hh_report->suspicious);
    }
    if (hh_args.kill_suspicious) {
       process_util::kill_suspicious(hh_report->suspicious);
    }
}
