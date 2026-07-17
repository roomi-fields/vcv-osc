#pragma once
#include <deque>
#include <mutex>
#include <vector>
#include "OscMessage.hpp"

/** Thread-safe hand-off between the OSC network thread (producer) and the UI
 * thread (consumer).
 *
 * WHY THIS EXISTS — the whole plugin hinges on it: OSC datagrams arrive on a
 * dedicated network thread, but cables live in the app/UI layer and MUST only
 * be created/removed from the UI thread (touching them from the audio or a
 * random thread crashes Rack). So the network thread only ever *enqueues*
 * parsed messages here; the UI thread drains them in ModuleWidget::step().
 * The lock is held for O(1) push / O(n) swap-drain only — never across any
 * Rack API call. */
namespace vcvosc {

class CommandQueue {
public:
	void push(OscMessage msg) {
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push_back(std::move(msg));
	}

	/** Move all pending messages out under the lock, then return them so the
	 * caller processes them without holding the lock. */
	std::vector<OscMessage> drain() {
		std::vector<OscMessage> out;
		std::lock_guard<std::mutex> lock(mutex_);
		out.reserve(queue_.size());
		while (!queue_.empty()) {
			out.push_back(std::move(queue_.front()));
			queue_.pop_front();
		}
		return out;
	}

	size_t size() {
		std::lock_guard<std::mutex> lock(mutex_);
		return queue_.size();
	}

private:
	std::mutex mutex_;
	std::deque<OscMessage> queue_;
};

} // namespace vcvosc
