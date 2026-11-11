#pragma once

namespace rega
{

/// From here on this process, and every program it starts, can open no
/// network connection: only Unix sockets (tmux) and netlink ones (ss, for the
/// ssh link's rate) can still be created. rega reads untrusted files with
/// several decoders; one of them misbehaving must not be able to send what
/// the user can read anywhere. Call before any thread starts: the filter
/// covers the calling thread and those created after it.
///
/// Linux (x86_64, aarch64) only, through seccomp. False where that is not
/// available (another system, an old kernel, a container that forbids it):
/// rega then runs as before.
bool denyNetwork();

/// $REGA_LOCAL_ONLY set (and not "0"): a site's rule that what rega shows
/// stays on the machine holding the files. rega then does not run when its
/// terminal is on another machine (remoteSession()), and never sends a copy
/// to the terminal's clipboard. A guard against doing so by accident, not
/// against a user who unsets the variable.
bool localOnly();

} // namespace rega
