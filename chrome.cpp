#include <string>
#include <vector>
#include <regex>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#else
#include <csignal>
#endif

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
#include "lib/tinyprocess/process.hpp"
#include "api/app/app.h"
#include "api/events/events.h"
#include "api/debug/debug.h"

#include "lib/json/json.hpp"
#include "lib/filedialogs/portable-file-dialogs.h"
#include "settings.h"
#include "helpers.h"
#include "api/os/os.h"
#include "api/fs/fs.h"

using namespace std;
using json = nlohmann::json;

/*
* C++ rewrite of https://github.com/zserge/lorca/ (Go-based) concept
*/

namespace chrome {

using DevToolsClient = websocketpp::client<websocketpp::config::asio_client>;
unique_ptr<TinyProcessLib::Process> browserProcess;
unique_ptr<DevToolsClient> devTools;
thread controlThread;
thread watcherThread;
#if defined(_WIN32)
HANDLE browserJob = nullptr;
#endif
websocketpp::connection_hdl browserConnection;
atomic<int> commandId{10};
atomic<bool> connected{false};
atomic<bool> connectionFailed{false};
atomic<bool> navigated{false};
atomic<bool> closing{false};
string pageSession;
string navigationUrl;
string bootstrapUrl;
bool attaching = false;
bool interceptClose = false;

int sendCommand(const string &method, const json &params = json::object(),
                const string &session = "", int id = 0) {
    if(!connected) return 0;
    if(!id) id = commandId++;
    json message = {{"id", id}, {"method", method}, {"params", params}};
    if(!session.empty()) message["sessionId"] = session;
    websocketpp::lib::error_code error;
    devTools->send(browserConnection, message.dump(), websocketpp::frame::opcode::text, error);
    return error ? 0 : id;
}

// Installed before navigating to the application. The client may explicitly
// allow a reload; OS close requests otherwise remain under application control.
const string closeGuard = R"JS(
window.NL_CHROME_CLOSE_GUARD = true;
window.addEventListener('neuDev_reloadApp', () => { window.NL_CHROME_ALLOW_NAVIGATION = true; });
window.addEventListener('beforeunload', event => {
    if(window.NL_CHROME_ALLOW_NAVIGATION) return;
    event.preventDefault();
    event.returnValue = true;
});
)JS";

void attachPage(const json &target) {
    if(attaching || target["type"] != "page" || target["url"] != bootstrapUrl) return;
    attaching = true;
    sendCommand("Target.attachToTarget", {
        {"targetId", target["targetId"]}, {"flatten", true}
    }, "", 2);
}

void handleDevToolsMessage(const json &message) {
    if(message.contains("error")) {
        connectionFailed = true;
        debug::log(debug::LogTypeError, "Chrome control error: " + message.dump());
        return;
    }
    int id = message.value("id", 0);
    if(id == 1) {
        for(const auto &target: message["result"]["targetInfos"]) attachPage(target);
    }
    else if(id == 2) {
        pageSession = message["result"]["sessionId"].get<string>();
        sendCommand("Page.enable", json::object(), pageSession, 3);
    }
    else if(id == 3) {
        if(interceptClose)
            sendCommand("Page.addScriptToEvaluateOnNewDocument", {{"source", closeGuard}}, pageSession, 4);
        else
            sendCommand("Page.navigate", {{"url", navigationUrl}}, pageSession, 6);
    }
    else if(id == 4) {
        sendCommand("Page.navigate", {{"url", navigationUrl}}, pageSession, 6);
    }
    else if(id == 6) {
        if(message["result"].contains("errorText")) connectionFailed = true;
        else {
            sendCommand("Page.resetNavigationHistory", json::object(), pageSession);
            navigated = true;
        }
    }
    else if(id == 5 && !closing) {
        events::dispatchToAllApps("windowClose", nullptr);
    }
    string method = message.value("method", "");
    if(method == "Target.targetCreated" || method == "Target.targetInfoChanged")
        attachPage(message["params"]["targetInfo"]);
    if(method == "Page.domContentEventFired" && interceptClose) {
        // Establish activation even when the first action is the OS close button.
        sendCommand("Runtime.evaluate", {{"expression", "void 0"}, {"userGesture", true}}, pageSession);
    }
    else if(method == "Page.javascriptDialogOpening" && interceptClose && !closing &&
            message["params"]["type"] == "beforeunload") {
        // With Page enabled Chrome delegates the dialog to this client. Cancel
        // it before notifying the app, so its progress UI can continue rendering.
        sendCommand("Page.handleJavaScriptDialog", {{"accept", false}}, pageSession, 5);
    }
}

void connectDevTools(const string &profilePath) {
    string port, endpoint;
    for(int attempt = 0; attempt < 200; attempt++) {
        ifstream activePort(filesystem::u8path(profilePath + "/DevToolsActivePort"));
        if(getline(activePort, port) && getline(activePort, endpoint) && !endpoint.empty()) break;
        int status;
        if(browserProcess->try_get_exit_status(status))
            throw runtime_error("Chrome exited before its control connection was ready (profile may be in use)");
        this_thread::sleep_for(chrono::milliseconds(50));
    }
    if(port.empty() || endpoint.empty()) throw runtime_error("Timed out connecting to Chrome");
    devTools = make_unique<DevToolsClient>();
    devTools->clear_access_channels(websocketpp::log::alevel::all);
    devTools->clear_error_channels(websocketpp::log::elevel::all);
    devTools->init_asio();
    devTools->set_open_handler([](websocketpp::connection_hdl handle) {
        browserConnection = handle;
        connected = true;
        sendCommand("Target.setDiscoverTargets", {{"discover", true}});
        sendCommand("Target.getTargets", json::object(), "", 1);
    });
    devTools->set_message_handler([](websocketpp::connection_hdl, DevToolsClient::message_ptr message) {
        try { handleDevToolsMessage(json::parse(message->get_payload())); }
        catch(const exception &error) { debug::log(debug::LogTypeError, error.what()); }
    });
    devTools->set_fail_handler([](websocketpp::connection_hdl) { connectionFailed = true; });
    devTools->set_close_handler([](websocketpp::connection_hdl) { connected = false; });
    websocketpp::lib::error_code error;
    auto connection = devTools->get_connection("ws://127.0.0.1:" + port + endpoint, error);
    if(error) throw runtime_error(error.message());
    devTools->connect(connection);
    controlThread = thread([]() { devTools->run(); });
    for(int attempt = 0; attempt < 200 && !navigated && !connectionFailed; attempt++)
        this_thread::sleep_for(chrono::milliseconds(50));
    if(!navigated) throw runtime_error("Chrome control connection could not initialize the application page");
}

void close() {
    if(closing.exchange(true) || !browserProcess) return;
    // Browser.close bypasses the application's beforeunload guard. Fall back to
    // terminating only the owned process group if Chrome is unresponsive.
    sendCommand("Browser.close");
    int status;
    bool exited = false;
    for(int attempt = 0; attempt < 60; attempt++) {
        if(browserProcess->try_get_exit_status(status)) { exited = true; break; }
        this_thread::sleep_for(chrono::milliseconds(50));
    }
    if(!exited) {
        #if defined(_WIN32)
        if(browserJob) TerminateJobObject(browserJob, 0);
        else browserProcess->kill(true);
        #else
        browserProcess->signal(SIGKILL);
        #endif
        browserProcess->get_exit_status();
    }
    #if defined(_WIN32)
    if(browserJob) { CloseHandle(browserJob); browserJob = nullptr; }
    #endif
    if(watcherThread.joinable()) {
        if(watcherThread.get_id() == this_thread::get_id()) watcherThread.detach();
        else watcherThread.join();
    }
    if(devTools) devTools->stop();
    if(controlThread.joinable()) controlThread.join();
}


string __getDefaultChromeArgs() {
    return "--disable-background-networking "
    "--disable-background-timer-throttling "
    "--disable-backgrounding-occluded-windows "
    "--disable-breakpad "
    "--disable-client-side-phishing-detection "
    "--disable-default-apps "
    "--disable-dev-shm-usage "
    "--disable-infobars "
    "--disable-extensions "
    "--disable-features=site-per-process "
    "--disable-hang-monitor "
    "--disable-ipc-flooding-protection "
    "--disable-popup-blocking "
    "--disable-prompt-on-repost "
    "--disable-renderer-backgrounding "
    "--disable-sync "
    "--disable-translate "
    "--disable-windows10-custom-titlebar "
    "--metrics-recording-only "
    "--no-first-run "
    "--no-default-browser-check "
    "--safebrowsing-disable-auto-update "
    "--password-store=basic "
    "--use-mock-keychain";
}

string __findChrome(const json &input) {
    string chromePath = "";
    #if defined(__linux__)
    vector<string> chromeBins = {
        "/usr/bin/google-chrome-stable",
        "/usr/bin/google-chrome",
        "/usr/bin/chromium",
        "/usr/bin/chromium-browser",
        "/snap/bin/chromium",
    };

    #elif defined(__APPLE__)
    vector<string> chromeBins = {
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
        "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
        "/usr/bin/google-chrome-stable",
        "/usr/bin/google-chrome",
        "/usr/bin/chromium",
        "/usr/bin/chromium-browser",
    };

    #elif defined(_WIN32)
    vector<string> chromeBins = {
        os::getEnv("LocalAppData") + "/Google/Chrome/Application/chrome.exe",
        os::getEnv("ProgramFiles") + "/Google/Chrome/Application/chrome.exe",
        os::getEnv("ProgramFiles(x86)") + "/Google/Chrome/Application/chrome.exe",
        os::getEnv("LocalAppData") + "/Chromium/Application/chrome.exe",
        os::getEnv("ProgramFiles") + "/Chromium/Application/chrome.exe",
        os::getEnv("ProgramFiles(x86)") + "/Chromium/Application/chrome.exe",
        os::getEnv("ProgramFiles(x86)") + "/Microsoft/Edge/Application/msedge.exe",
        os::getEnv("ProgramFiles") + "/Microsoft/Edge/Application/msedge.exe",
    };
    #endif

    string browserBinaryKeyForOs = "browserBinary" + string(NEU_OS_NAME);
    if(helpers::hasField(input, "browserBinary") || helpers::hasField(input, browserBinaryKeyForOs)) {
        string customBin = helpers::hasField(input, browserBinaryKeyForOs) ? input[browserBinaryKeyForOs].get<string>() : 
                                    input["browserBinary"].get<string>();
        chromeBins.insert(chromeBins.begin(), fs::applyPathConstants(customBin));
    }

    for(const string &cmd: chromeBins) {
        fs::FileStats stats = fs::getStats(cmd);
        if(stats.status == errors::NE_ST_OK && stats.entryType == fs::EntryTypeFile) {
            chromePath = cmd;
            break;
        }
    }
    return chromePath;
}

void init(const json &input) {

    string chromeCmd = __findChrome(input);

    if(chromeCmd.empty()) {
        pfd::message("Unable to start Chrome mode",
                        "You need to install Google Chrome to run this Neutralinojs application",
                        pfd::choice::ok,
                        pfd::icon::error);
        std::exit(1);
    }

    if(regex_search(chromeCmd, regex(" "))) {
        chromeCmd = "\"" + chromeCmd + "\"";
    }

    chromeCmd += " " + __getDefaultChromeArgs();

    string profilePath = input.value("userDataDir", settings::joinAppDataPath("/.tmp/chromedata"));
    navigationUrl = input["url"].get<string>();
    interceptClose = !input.value("exitProcessOnClose", true);
    filesystem::create_directories(filesystem::u8path(profilePath));
    error_code removeError;
    filesystem::remove(filesystem::u8path(profilePath + "/DevToolsActivePort"), removeError);
    chromeCmd += " --user-data-dir=\"" + profilePath + "\"";
    bootstrapUrl = navigationUrl.substr(0, navigationUrl.find('/', navigationUrl.find("://") + 3)) +
                    "/__neutralino_chrome_bootstrap";
    chromeCmd += " --app=\"" + bootstrapUrl + "\"";

    if(helpers::hasRequiredFields(input, {"width", "height"})) {
        chromeCmd += " --window-size=" + to_string(input["width"].get<int>()) +
                    "," + to_string(input["height"].get<int>());
    }

    if(helpers::hasField(input, "args")) {
        chromeCmd += " " + input["args"].get<string>();
    }

    // The private loopback DevTools endpoint controls only this app's browser.
    chromeCmd += " --remote-debugging-address=127.0.0.1 --remote-debugging-port=0";
    browserProcess = make_unique<TinyProcessLib::Process>(CONVSTR(chromeCmd));
    #if defined(_WIN32)
    // Keep Chromium's descendants in the same owned job, including the fallback
    // path where an unresponsive browser cannot process Browser.close.
    browserJob = CreateJobObject(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, browserProcess->get_id());
    if(!browserJob || !process ||
            !SetInformationJobObject(browserJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(browserJob, process)) {
        debug::log(debug::LogTypeError, "Unable to assign Chrome to its process cleanup job");
        if(browserJob) CloseHandle(browserJob);
        browserJob = nullptr;
    }
    if(process) CloseHandle(process);
    #endif
    try {
        connectDevTools(profilePath);
        watcherThread = thread([]() {
            while(!closing) {
                int status;
                if(browserProcess->try_get_exit_status(status)) { app::exit(status); return; }
                this_thread::sleep_for(chrono::milliseconds(100));
            }
        });
    }
    catch(const exception &error) {
        debug::log(debug::LogTypeError, error.what());
        close();
        std::exit(1);
    }
}

} // namespace chrome

