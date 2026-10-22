#pragma once

#include "image/ImageBuffer.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rv
{

using ImageInfoPtr = std::shared_ptr<const ImageInfo>;

/// Asynchronous, cached access to image headers and decoded layers. Every
/// getter is non-blocking: it returns what is cached (or nullptr) and schedules
/// the missing work on a small thread pool; `notify` is called (from a worker
/// thread) whenever something asked for with Priority::NOW lands. Prefetches
/// land quietly: nothing on screen waits for them.
class ImageService
{
public:
	enum class Priority
	{
		NOW,
		PREFETCH
	};

	static constexpr size_t DEFAULT_BUDGET = size_t(4) << 30; ///< 4 GiB

	ImageService(std::function<void()> notify,
	             size_t budgetBytes = DEFAULT_BUDGET,
	             int threads = 3);
	~ImageService();

	ImageInfoPtr info(const std::filesystem::path& p,
	                  Priority pr = Priority::NOW);

	/// Layer selected by label (stable across sequence frames); falls back to
	/// the first layer. `reduce` > 1 caches a box-downsampled copy (preview /
	/// playback).
	LayerImagePtr layer(const std::filesystem::path& p,
	                    const std::string& layerLabel,
	                    int reduce,
	                    Priority pr = Priority::NOW);

	/// Like layer(), but if the requested resolution isn't ready yet returns
	/// any cached resolution of the same layer (best first) so the view never
	/// goes blank.
	LayerImagePtr layerBestEffort(const std::filesystem::path& p,
	                              const std::string& layerLabel,
	                              int reduce,
	                              Priority pr = Priority::NOW);

	/// |A - B| of a layer (see diffLayers): both decoded at full resolution
	/// (and cached so), then reduced. nullptr until it lands, or after an
	/// error (see diffError()).
	LayerImagePtr diffLayer(const std::filesystem::path& a,
	                        const std::filesystem::path& b,
	                        const std::string& layerLabel,
	                        int reduce,
	                        Priority pr = Priority::NOW);
	std::optional<std::string> diffError(const std::filesystem::path& a,
	                                     const std::filesystem::path& b);

	bool hasLayer(const std::filesystem::path& p,
	              const std::string& layerLabel,
	              int reduce);
	/// The LayerImage::m_origin layer() gives for these, without decoding:
	/// whether the terminal already keeps that frame (ImageSlot::stored).
	std::string originOf(const std::filesystem::path& p,
	                     const std::string& layerLabel,
	                     int reduce);

	/// For each of `paths`, whether `layerLabel` is decoded at any
	/// resolution (the playbar's cached frames). One pass over the cache;
	/// files never asked for are not stat'ed.
	std::vector<bool>
	cachedLayers(const std::vector<std::filesystem::path>& paths,
	             const std::string& layerLabel);

	std::optional<std::string> error(const std::filesystem::path& p);

	/// Re-stat `paths`; any whose mtime/size changed (re-rendered, overwritten,
	/// deleted) has its cached header, layers and load error dropped, so the
	/// next request decodes the new contents. Returns true if anything changed.
	bool checkForChanges(const std::vector<std::filesystem::path>& paths);

	/// Make a cached layer the next to go when room is needed. Playback
	/// calls it for the frame it just left: a loop needs that frame again
	/// last, so a sequence larger than the cache keeps its first frames
	/// between loops instead of evicting each one just before it comes
	/// round (what plain LRU does to a loop). No-op when not cached.
	void demote(const std::filesystem::path& p,
	            const std::string& layerLabel,
	            int reduce);

	/// Drop queued prefetch jobs (e.g. when the playback range or selection
	/// changes).
	void clearPrefetch();

	size_t bytesUsed();
	size_t budget() const
	{
		return m_budget;
	}
	size_t pendingJobs();

private:
	struct Job
	{
		std::string m_key;
		std::string m_ident; ///< path|stamp: what errors and headers key on
		std::filesystem::path m_path;
		std::optional<std::string> m_layer; ///< nullopt: header only
		int m_reduce = 1;
		Priority m_pr;
		/// A diff: A's file and ident (m_path, m_ident: B's, and the pair's).
		std::optional<std::filesystem::path> m_diffA;
		std::string m_identA, m_identB;
	};
	struct CacheEntry
	{
		std::variant<ImageInfoPtr, LayerImagePtr> m_value;
		size_t m_bytes = 0;
		std::list<std::string>::iterator m_lru;
	};

	/// "path|mtime:size" for `p`; stats the file the first time it is seen.
	/// Every cache key includes it, so a changed file is a new image. m_mu held.
	std::string identLocked(const std::filesystem::path& p);
	static std::string layerKey(const std::string& ident,
	                            const std::string& layer,
	                            int reduce);
	/// "D|identA|identB": what a diff's errors key on. m_mu held.
	std::string diffIdentLocked(const std::filesystem::path& a,
	                            const std::filesystem::path& b);
	void enqueue(Job job);
	void run();
	/// From the cache, else decoded (and cached). Worker threads only.
	ImageInfoPtr infoCached(const std::filesystem::path& p,
	                        const std::string& ident);
	LayerImagePtr layerCached(const std::filesystem::path& p,
	                          const std::string& ident,
	                          const std::string& label,
	                          int reduce);
	void put(const std::string& key, CacheEntry entry);
	template <class T>
	std::shared_ptr<const T> get(const std::string& key);

	std::function<void()> m_notify;
	size_t m_budget;
	std::mutex m_mu;
	std::condition_variable m_cv;
	std::deque<Job> m_queue;
	/// Queued or running, by key; true once asked for with Priority::NOW.
	std::unordered_map<std::string, bool> m_inFlight;
	std::unordered_map<std::string, CacheEntry> m_cache;
	std::list<std::string> m_lru; ///< front = most recent
	std::unordered_map<std::string, std::string> m_errors; ///< by ident
	std::unordered_map<std::string, std::string> m_stamps; ///< path → stamp
	size_t m_bytes = 0;
	bool m_stop = false;
	std::vector<std::thread> m_threads;
};

} // namespace rv
