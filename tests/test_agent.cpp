// Agent mode prototype: the wire protocol and the RAM-only frame store.

#include "agent/Agent.h"
#include "agent/AgentClient.h"
#include "agent/Protocol.h"

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace rega::agent;

namespace
{

// The bytes shared memory object `name` holds; empty when it doesn't exist.
std::string shmBytes(const std::string& name, size_t n)
{
	const int fd = shm_open(name.c_str(), O_RDONLY, 0);
	if(fd < 0)
	{
		return {};
	}
	void* p = mmap(nullptr, n, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if(p == MAP_FAILED)
	{
		return {};
	}
	std::string out(static_cast<const char*>(p), n);
	munmap(p, n);
	return out;
}

bool exists(const std::string& name)
{
	const int fd = shm_open(name.c_str(), O_RDONLY, 0);
	if(fd >= 0)
	{
		close(fd);
	}
	return fd >= 0;
}

std::vector<uint8_t> pixels(size_t n, uint8_t seed)
{
	std::vector<uint8_t> px(n);
	for(size_t i = 0; i < n; ++i)
	{
		px[i] = static_cast<uint8_t>(i * 7 + seed);
	}
	return px;
}

std::string token()
{
	// Unique per test run: names are global to the machine.
	return "t" + std::to_string(getpid() % 100000);
}

std::vector<Message> everyKind()
{
	Message hello;
	hello.m_type = Type::HELLO;
	hello.m_version = PROTOCOL_VERSION;
	hello.m_token = "abc123";
	Message frame;
	frame.m_type = Type::FRAME;
	frame.m_key = 41;
	frame.m_width = 3;
	frame.m_height = 2;
	frame.m_codec = Codec::JPEG;
	frame.m_bytes = std::string("\xff\xd8\0\x01", 4);
	Message ready;
	ready.m_type = Type::READY;
	ready.m_key = 41;
	ready.m_name = "/reabc123-41-0";
	ready.m_size = uint64_t(1) << 33;
	Message drop;
	drop.m_type = Type::DROP;
	drop.m_key = 7;
	Message shown;
	shown.m_type = Type::SHOWN;
	shown.m_key = 9;
	Message bye;
	bye.m_type = Type::BYE;
	return {hello, frame, ready, drop, shown, bye};
}

} // namespace

TEST(AgentProtocol, EveryMessageRoundTripsOverASocket)
{
	int sv[2];
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
	const auto sent = everyKind();
	for(const auto& m : sent)
	{
		ASSERT_TRUE(writeMessage(sv[0], m));
	}
	for(const auto& m : sent)
	{
		const auto got = readMessage(sv[1]);
		ASSERT_TRUE(got);
		EXPECT_EQ(*got, m);
	}
	close(sv[0]);
	EXPECT_FALSE(readMessage(sv[1])); // EOF
	close(sv[1]);
}

TEST(AgentProtocol, ReaderTakesMessagesSplitAnywhere)
{
	std::string wire;
	for(const auto& m : everyKind())
	{
		wire += encode(m);
	}
	int sv[2];
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
	fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);
	Reader r;
	std::vector<Message> got;
	for(size_t i = 0; i < wire.size(); i += 3) // a few bytes at a time
	{
		ASSERT_TRUE(writeAll(sv[0], std::string_view(wire).substr(i, 3)));
		ASSERT_TRUE(r.fill(sv[1]));
		while(auto m = r.next())
		{
			got.push_back(*m);
		}
	}
	EXPECT_FALSE(r.failed());
	EXPECT_EQ(got, everyKind());
	close(sv[0]);
	EXPECT_FALSE(r.fill(sv[1])); // EOF
	close(sv[1]);
}

TEST(AgentProtocol, MalformedIsRejected)
{
	std::string frame = encode(everyKind()[1]);
	EXPECT_FALSE(decode(std::string_view(frame).substr(4, frame.size() - 5)));
	EXPECT_FALSE(decode(std::string(frame.substr(4)) + "x"));
	EXPECT_FALSE(decode(std::string("\x63", 1))); // no such type
	Reader r;
	int sv[2];
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
	fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);
	ASSERT_TRUE(writeAll(sv[0], std::string("\xff\xff\xff\xff", 4)));
	ASSERT_TRUE(r.fill(sv[1]));
	EXPECT_FALSE(r.next());
	EXPECT_TRUE(r.failed()); // past MAX_MESSAGE
	close(sv[0]);
	close(sv[1]);
}

TEST(AgentStore, RearmsUnderTheNextNameOnceTheTerminalUnlinks)
{
	ShmStore store(token(), 0);
	const auto px = pixels(4096 * 3, 1);
	const auto put = store.put(5, px.data(), px.size());
	ASSERT_FALSE(put.m_name.empty());
	EXPECT_LE(put.m_name.size(), 31u); // macOS
	const std::string want(px.begin(), px.end());
	EXPECT_EQ(shmBytes(put.m_name, px.size()), want);

	store.watch(5);
	EXPECT_TRUE(store.rearm().empty()); // not read yet
	// What the terminal does once it read the picture.
	ASSERT_EQ(shm_unlink(put.m_name.c_str()), 0);
	const auto rearmed = store.rearm();
	ASSERT_EQ(rearmed.size(), 1u);
	EXPECT_EQ(rearmed[0].first, 5u);
	EXPECT_NE(rearmed[0].second, put.m_name);
	EXPECT_EQ(rearmed[0].second, store.name(5));
	EXPECT_EQ(shmBytes(rearmed[0].second, px.size()), want);
	EXPECT_FALSE(store.watching()); // until shown again
	EXPECT_TRUE(store.rearm().empty());
	EXPECT_EQ(store.bytes(), px.size());
}

TEST(AgentStore, ClearAndDestructionUnlinkEverything)
{
	std::vector<std::string> names;
	{
		ShmStore store(token(), 0);
		const auto px = pixels(1024, 2);
		for(uint32_t key = 0; key < 3; ++key)
		{
			names.push_back(store.put(key, px.data(), px.size()).m_name);
			ASSERT_TRUE(exists(names.back()));
		}
		store.drop(1);
		EXPECT_FALSE(exists(names[1]));
		EXPECT_EQ(store.frames(), 2u);
		store.clear();
		EXPECT_EQ(store.bytes(), 0u);
		names.push_back(store.put(9, px.data(), px.size()).m_name);
	}
	for(const auto& name : names)
	{
		EXPECT_FALSE(exists(name)) << name;
	}
}

TEST(AgentStore, BudgetDropsTheLeastRecentlyUsed)
{
	const auto px = pixels(1000, 3);
	ShmStore store(token(), 2500);
	store.put(1, px.data(), px.size());
	store.put(2, px.data(), px.size());
	// 2's re-arm makes 1 the oldest... and so does showing 1 again later.
	store.watch(1);
	shm_unlink(store.name(1).c_str());
	ASSERT_EQ(store.rearm().size(), 1u); // 1 is now the newest
	const auto put = store.put(3, px.data(), px.size());
	ASSERT_EQ(put.m_dropped, std::vector<uint32_t>{2});
	EXPECT_TRUE(store.name(2).empty());
	EXPECT_FALSE(store.name(1).empty());
	EXPECT_EQ(store.frames(), 2u);
	EXPECT_LE(store.bytes(), 2500u);
}

TEST(AgentStore, ReplacingAKeyUnlinksItsOldName)
{
	ShmStore store(token(), 0);
	const auto a = pixels(512, 4), b = pixels(512, 5);
	const std::string first = store.put(3, a.data(), a.size()).m_name;
	const std::string second = store.put(3, b.data(), b.size()).m_name;
	EXPECT_NE(first, second);
	EXPECT_FALSE(exists(first));
	EXPECT_EQ(shmBytes(second, b.size()), std::string(b.begin(), b.end()));
	EXPECT_EQ(store.frames(), 1u);
}

TEST(AgentSsh, ForwardAndEnvGoBeforeTheDestination)
{
	const auto cmd = sshCommand({"ssh", "-t", "ldn", "tmux", "new", "-A"},
	                            "/tmp/r.sock",
	                            "/var/l.sock");
	const std::vector<std::string> want{"ssh",
	                                    "-R",
	                                    "/tmp/r.sock:/var/l.sock",
	                                    "-o",
	                                    "SetEnv=LC_REGA_AGENT=/tmp/r.sock",
	                                    "-o",
	                                    "SendEnv=LC_REGA_AGENT",
	                                    "-t",
	                                    "ldn",
	                                    "tmux",
	                                    "new",
	                                    "-A"};
	EXPECT_EQ(cmd, want);
	EXPECT_TRUE(sshCommand({}, "a", "b").empty());
}

TEST(AgentShellInit, WrapsInteractiveSshWithTheQuotedPath)
{
	const std::string bash = shellInit("bash", "/opt/it's here/rega");
	EXPECT_NE(bash.find("rega_ssh() {"), std::string::npos);
	EXPECT_NE(bash.find("|| ssh() { rega_ssh \"$@\"; }"), std::string::npos);
	EXPECT_NE(bash.find("'/opt/it'\\''s here/rega' --agent -- ssh \"$@\""),
	          std::string::npos);
	EXPECT_NE(bash.find("command ssh \"$@\""), std::string::npos);
	EXPECT_NE(bash.find("REGA_NO_AGENT"), std::string::npos);
	EXPECT_EQ(shellInit("zsh", "/opt/it's here/rega"), bash);
	EXPECT_TRUE(shellInit("fish", "/x").empty());
}

namespace
{
// The agent end of a socketpair, scripted by the test.
struct FakeAgent
{
	int m_fd = -1;
	Message next()
	{
		auto m = readMessage(m_fd);
		EXPECT_TRUE(m);
		return m ? *m : Message{};
	}
	void ready(uint32_t key, const std::string& name)
	{
		Message m;
		m.m_type = Type::READY;
		m.m_key = key;
		m.m_name = name;
		ASSERT_TRUE(writeMessage(m_fd, m));
	}
};

// Until `done` (called until it is true once: it may have effects) or a
// second has gone.
template <class F>
bool eventually(F done)
{
	for(int i = 0; i < 200; ++i)
	{
		if(done())
		{
			return true;
		}
		usleep(5000);
	}
	return false;
}
} // namespace

TEST(AgentClient, ShowsEachArmedNameOnceThenWaitsForTheNext)
{
	int sv[2];
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
	FakeAgent agent{sv[1]};
	auto client = std::make_shared<AgentClient>(AgentLink{sv[0], "tok"}, "p");
	client->setQuality(QUALITY_LOSSLESS);
	rega::Rgba8Image img;
	img.m_width = 3;
	img.m_height = 2;
	img.m_pixels.assign(3 * 2 * 4, 200);
	const uint32_t key = client->send(img);
	ASSERT_NE(key, 0u);
	const Message frame = agent.next();
	EXPECT_EQ(frame.m_type, Type::FRAME);
	EXPECT_EQ(frame.m_key, key);
	EXPECT_EQ(frame.m_width, 3u);
	EXPECT_EQ(frame.m_codec, Codec::PNG);

	std::string name;
	EXPECT_EQ(client->take(key, name), AgentClient::Show::WAIT); // not yet
	agent.ready(key, "/retok-1-0");
	ASSERT_TRUE(eventually(
	    [&] { return client->take(key, name) == AgentClient::Show::NOW; }));
	EXPECT_EQ(name, "/retok-1-0");
	EXPECT_EQ(agent.next().m_type, Type::SHOWN);
	// Used up until the agent arms the next name.
	EXPECT_EQ(client->take(key, name), AgentClient::Show::WAIT);
	agent.ready(key, "/retok-1-1");
	ASSERT_TRUE(eventually(
	    [&] { return client->take(key, name) == AgentClient::Show::NOW; }));
	EXPECT_EQ(name, "/retok-1-1");
	EXPECT_EQ(agent.next().m_type, Type::SHOWN);

	client->forget(key);
	const Message forgot = agent.next();
	EXPECT_EQ(forgot.m_type, Type::DROP);
	EXPECT_EQ(forgot.m_key, key);
	EXPECT_EQ(client->take(key, name), AgentClient::Show::GONE);

	close(sv[1]); // the agent goes
	EXPECT_TRUE(eventually([&] { return !client->connected(); }));
	EXPECT_EQ(client->send(img), 0u);
}

TEST(AgentClient, HandsAnUnreadNameOutAgainAndDropMeansGone)
{
	int sv[2];
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
	FakeAgent agent{sv[1]};
	AgentClient client(AgentLink{sv[0], "tok"}, "p");
	rega::Rgba8Image img;
	img.m_width = img.m_height = 4;
	img.m_pixels.assign(4 * 4 * 4, 9);
	const uint32_t a = client.send(img), b = client.send(img);
	EXPECT_EQ(agent.next().m_codec, Codec::JPEG); // the default, q85
	agent.next();
	agent.ready(a, "/retok-1-0");
	std::string name;
	ASSERT_TRUE(eventually(
	    [&] { return client.take(a, name) == AgentClient::Show::NOW; }));
	// The terminal never read it (no re-arm): after STALE_AFTER, again.
	EXPECT_EQ(client.take(a, name), AgentClient::Show::WAIT);
	usleep(1100 * 1000);
	EXPECT_EQ(client.take(a, name), AgentClient::Show::NOW);
	EXPECT_EQ(name, "/retok-1-0");
	EXPECT_EQ(client.stats().m_stale, 1u);

	Message drop;
	drop.m_type = Type::DROP;
	drop.m_key = b;
	ASSERT_TRUE(writeMessage(sv[1], drop));
	EXPECT_TRUE(eventually(
	    [&] { return client.take(b, name) == AgentClient::Show::GONE; }));
	close(sv[1]);
}
