# Chrome lifecycle

Chrome is launched as an owned child process. The framework connects to its
private loopback DevTools endpoint before navigating from a blank bootstrap
page to the application. `app.exit()` asks Chrome to close, waits up to three
seconds, and then terminates the owned process group (Windows: job) if necessary.
A normal browser exit also terminates the backing Neutralino process promptly.

Set `modes.chrome.exitProcessOnClose` to `false` to defer page close requests.
The framework installs a beforeunload guard before application scripts run,
cancels its DevTools dialog without showing the browser confirmation, and emits
`windowClose` to the application. The page remains alive, including rendering
and filesystem APIs. Applications should show progress, flush pending writes,
and call `Neutralino.app.exit()` only on success. Duplicate close requests may
occur; application shutdown handling should share a single pending operation.

`NL_CHROME_CLOSE_GUARD` is true when deferred-close support is installed.
For an intentional reload/navigation, set `NL_CHROME_ALLOW_NAVIGATION = true`
first. New documents install the guard again. Browser/task-manager force quit
and OS process termination cannot be deferred.

`modes.chrome.userDataDir` optionally selects a private profile directory, useful
for isolated smoke tests. A profile must not be shared by running instances.
The framework reserves an ephemeral loopback debugging port; additional Chrome
arguments must not override the profile directory. The DevToolsActivePort file
inside the profile exposes the endpoint to developer tooling owned by the user.

Linux is built/tested by Card Magician's `npm run smoke:exit`. That host-side suite
checks progress visibility, real image persistence before the debounce expires,
failed-write retry, reopened data, and the exit of both Neutralino and its owned
Chrome processes. Windows/macOS need native builds and the same suite before
release on those platforms.
