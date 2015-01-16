#include "motion_handler.h"
#include <algorithm>

class raw_input_consumer : public stream_consumer
{
public:
	friend class motion_handler;
	friend class motion_flag_consumer;

	raw_input_consumer()
		: stream_consumer("Motion Handling (Input)")
	{
	}
};

motion_handler::motion_handler()
	: stream_source("Motion Handling"), flag_stream(0), destroy_flag(false), prerecord_time(0), postrecord_time(0)
{
	raw_stream = new raw_input_consumer;
}

motion_handler::~motion_handler()
{
	delete raw_stream;
	delete flag_stream;
}

void motion_handler::destroy()
{
	destroy_flag = true;
	raw_stream->buffer_wait.notify_all();
}

void motion_handler::disconnect()
{
	raw_stream->disconnect();
	if (flag_stream)
		flag_stream->disconnect();
}

stream_consumer *motion_handler::input_consumer()
{
	return raw_stream;
}

void motion_handler::set_buffer_time(int pre, int post)
{
	std::lock_guard<std::mutex> l(raw_stream->lock);
	if (pre == prerecord_time && post == postrecord_time)
		return;

	prerecord_time = pre;
	postrecord_time = post;

	raw_stream->buffer.set_duration(prerecord_time);
}

void motion_handler::set_motion_analysis_stw(int64_t interval_mcs)
{
	stw_motion_analysis.setTimeWindow(interval_mcs);
}

void motion_handler::set_motion_analysis_percentage(int percentage)
{
	motion_threshold_percentage = percentage;
}

void motion_handler::run()
{
	std::unique_lock<std::mutex> l(raw_stream->lock);
	bool recording = false;
	time_t last_motion = 0;
	unsigned last_recorded_seq = 0;

	bc_log_context_push(log);

	int last_pkt_seq = -1;
	while (!destroy_flag)
	{
		raw_stream->buffer_wait.wait(l);
		if (raw_stream->buffer.empty())
			continue;

		const stream_keyframe_buffer &buffer = raw_stream->buffer;

		bool triggered = false;
		for (auto it = buffer.begin(); it != buffer.end(); it++) {
			if ((int)it->seq <= last_pkt_seq)
				continue;
			last_pkt_seq = it->seq;
			bc_log(Debug, "pkt: flags: 0x%02x, size: %u, pts: %" PRId64 " ts_clock: %u ts_monotonic: %u seq: %u", it->flags, it->size, it->pts, (unsigned)it->ts_clock, (unsigned)it->ts_monotonic, it->seq);
			// If N% of frames in the past X seconds contain motion, trigger
			// Push info on packet into sliding time window checker (STWC)
			// Drop from STWC the packets which don't fit to the specified window (done internally with push())
			bc_log(Debug, "pkt: motion: %s, pts: %" PRId64 " seq: %u", (it->flags & stream_packet::MotionFlag) ? "YES" : "NO ", it->pts, it->seq);
			// TODO Use DTS instead of PTS for STWC for sure monotonity?
			stw_motion_analysis.push(/* timestamp */ it->pts, /* value */ (it->flags & stream_packet::MotionFlag) ? 1 : 0);
			// Check the "sum" (count) of motion-flagged packets in STWC
			bc_log(Debug, "stw_motion_analysis.sum() = %" PRId64 "; stw_motion_analysis.count() = %" PRId64 "; percentage = %d; motion_threshold_percentage %d",
					stw_motion_analysis.sum(), stw_motion_analysis.count(),
					100 * stw_motion_analysis.sum() / stw_motion_analysis.count(),
					motion_threshold_percentage);
			if (100 * stw_motion_analysis.sum() / stw_motion_analysis.count() < motion_threshold_percentage)
				continue;
			// Note: STW analysis is reset on pause and stop.

			triggered = true;
			break;
		}

		int send_from = -1;

		if (triggered && !recording) {
			/* If the recording is paused, and the last recorded packet is still
			 * in the buffer, send all packets between that and the current packet
			 * to resume the recording. */
			if (last_recorded_seq && last_recorded_seq >= buffer.front().seq) {
				unsigned int resume_pos = 0;
				for (; resume_pos < buffer.size(); resume_pos++) {
					if (buffer[resume_pos].seq > last_recorded_seq)
						break;
				}

				bc_log(Debug, "motion: resume recording");
			} else {
				bc_log(Debug, "motion: start recording");
				send_from = 0;
			}
			recording = true;
		}

		if (!triggered && recording && buffer.back().ts_monotonic - last_motion > postrecord_time - prerecord_time) {
			bc_log(Debug, "motion: pause recording");
			recording = false;
			stw_motion_analysis.reset();  // Drains STW but preserves interval value
		}

		if (!recording && last_recorded_seq && buffer.front().seq > last_recorded_seq) {
			bc_log(Debug, "motion: stop recording");
			last_recorded_seq = 0;
			// Send null packet to end recording
			send(stream_packet());
			stw_motion_analysis.reset();  // Drains STW but preserves interval value
			continue;
		}

		if (triggered)
			last_motion = buffer.back().ts_monotonic;

		if (!recording)
			continue;

		if (send_from < 0) {
			for (int i = buffer.size()-1; i >= 0; i--) {
				if (buffer[i].seq > last_recorded_seq)
					continue;

				send_from = i+1;
				break;
			}

			if (send_from < 0) {
				bc_log(Bug, "motion_handler: last_recorded_seq is not in buffer");
				send_from = 0;
			}
		}

		for (unsigned int i = send_from; i < buffer.size(); i++)
			send(buffer[i]);
		last_recorded_seq = buffer.back().seq;

		// note lock is held
	}

	bc_log(Debug, "motion_handler destroying");
	l.unlock();
	delete this;
}

static inline bool packet_seq_compare(const stream_packet &p1, const stream_packet &p2)
{
	return p1.seq < p2.seq;
}

class motion_flag_consumer : public stream_consumer
{
public: 
	motion_flag_consumer(raw_input_consumer *target)
		: stream_consumer("Motion Handling (Flags)"), target(target)
	{
	}

	virtual void receive(const stream_packet &packet)
	{
		if (!target) {
			bc_log(Warning, "motion_flag_consumer ignoring packet due to missing target");
			return;
		}

		std::lock_guard<std::mutex> l(target->lock);
		auto it = std::lower_bound(target->buffer.begin(), target->buffer.end(),
		                           packet, packet_seq_compare);

		if (it == target->buffer.end()) {
			/* This case would mean that the entire motion process happened before
			 * the raw input was able to receive the packet; that's weird. To avoid
			 * double packets or the expense of having to check for raw stream inserts,
			 * this condition isn't supported. */
			bc_log(Bug, "motion_flag_consumer has a newer packet than target buffer");
			return;
		}

		if (it->seq == packet.seq) {
			it->flags = (it->flags & ~stream_packet::MotionFlag) | packet.flags;
		} else {
			bc_log(Debug, "motion_flag_consumer: missing packet %u in input (next is %u)", packet.seq, it->seq);
			// XXX trigger a recording anyway, starting as early as we can?
		}

		target->buffer_wait.notify_one();
	}

private:
	raw_input_consumer *target;
};

stream_consumer *motion_handler::create_flag_consumer()
{
	std::lock_guard<std::mutex> l(lock);
	if (!flag_stream)
		flag_stream = new motion_flag_consumer(raw_stream);
	return flag_stream;
}

