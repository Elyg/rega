#include "util/Sandbox.h"

#include <cstdlib>
#include <string_view>

namespace rega
{

bool localOnly()
{
	const char* v = std::getenv("REGA_LOCAL_ONLY");
	return v && *v && std::string_view(v) != "0";
}

} // namespace rega

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>

#include <cerrno>
#include <cstddef>

namespace rega
{

bool denyNetwork()
{
#if defined(__x86_64__)
	constexpr unsigned ARCH = AUDIT_ARCH_X86_64;
#else
	constexpr unsigned ARCH = AUDIT_ARCH_AARCH64;
#endif
	constexpr unsigned REFUSE = SECCOMP_RET_ERRNO | EPERM;
	constexpr unsigned NO_NETWORK = SECCOMP_RET_ERRNO | EACCES;
	// The first argument's low half: socket()'s address family.
	constexpr unsigned FAMILY = offsetof(seccomp_data, args);

	sock_filter filter[] = {
	    // System calls of another ABI (32-bit ones on x86_64) have other
	    // numbers: refused, rather than read as something they are not.
	    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
	    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ARCH, 1, 0),
	    BPF_STMT(BPF_RET | BPF_K, REFUSE),
	    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
	    // x32 calls share x86_64's ABI but not its numbers.
	    BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1),
	    BPF_STMT(BPF_RET | BPF_K, REFUSE),
#ifdef __NR_io_uring_setup
	    // io_uring can create sockets without calling socket().
	    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_setup, 0, 1),
	    BPF_STMT(BPF_RET | BPF_K, REFUSE),
#endif
	    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 1, 0),
	    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, FAMILY),
	    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 2, 0),
	    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_NETLINK, 1, 0),
	    BPF_STMT(BPF_RET | BPF_K, NO_NETWORK),
	    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	sock_fprog program{};
	program.len = static_cast<unsigned short>(sizeof filter / sizeof filter[0]);
	program.filter = filter;
	// Needed to install a filter unprivileged; also stops setuid programs
	// started from here gaining anything.
	return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
	       prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

} // namespace rega

#else

namespace rega
{

bool denyNetwork()
{
	return false;
}

} // namespace rega

#endif
