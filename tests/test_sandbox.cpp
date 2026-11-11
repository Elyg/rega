#include "util/Sandbox.h"

#include <gtest/gtest.h>
#include <sys/socket.h>

#include <cerrno>
#include <unistd.h>

// Applies to the rest of this test process too: no test opens a connection.
TEST(Sandbox, InternetSocketsAreRefusedLocalOnesAreNot)
{
	if(!rega::denyNetwork())
	{
		GTEST_SKIP() << "no seccomp here";
	}
	for(const int family : {AF_INET, AF_INET6})
	{
		for(const int type : {SOCK_STREAM, SOCK_DGRAM})
		{
			errno = 0;
			const int fd = socket(family, type, 0);
			EXPECT_EQ(fd, -1);
			EXPECT_EQ(errno, EACCES);
			if(fd >= 0)
			{
				close(fd);
			}
		}
	}
	const int local = socket(AF_UNIX, SOCK_STREAM, 0);
	EXPECT_GE(local, 0); // tmux
	if(local >= 0)
	{
		close(local);
	}
}
