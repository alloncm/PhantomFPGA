/* SPDX-License-Identifier: MIT */
/*
 * PhantomFPGA Viewer - YOUR IMPLEMENTATION
 *
 * This is the file you need to edit. Implement the 7 TODO methods below
 * to receive, validate, display, and record frames from the device.
 *
 * Read phantomfpga_view.h for the class interface and frame constants.
 *
 * Available members from the base class (PhantomFpgaViewer):
 *   client_        -- TcpClient with read_exact(buf, len, &running_)
 *   terminal_      -- Terminal with clear_screen(), cursor_home(), etc.
 *   frame_buffer_  -- std::array<uint8_t, 5120> for the current frame
 *   stats_         -- ViewerStats (frames_received, frames_dropped, etc.)
 *   running_       -- volatile bool, goes false on Ctrl+C
 *   record_path_   -- filename from --record flag (empty = no recording)
 *
 * Frame layout (frame::SIZE = 5120 bytes):
 *   Offset 0:    FrameHeader (16 bytes) -- magic, sequence, reserved
 *   Offset 16:   Payload (4995 bytes) -- frame data
 *   Offset 5116: CRC32 (4 bytes) -- IEEE 802.3
 *
 * Utility:
 *   CRC32::compute(data, len) -- returns uint32_t
 */

#include "phantomfpga_view.h"

#include <arpa/inet.h>

/* ----------------------------------------------------------------------- */
/* PhantomFpgaViewerImpl -- YOUR CODE GOES HERE                            */
/*                                                                         */
/* Implement the 7 TODO methods below. The base class handles TCP          */
/* connection, terminal setup, signal handling, and the main loop.         */
/* ----------------------------------------------------------------------- */

class PhantomFpgaViewerImpl : public PhantomFpgaViewer {
protected:
	bool receive_frame() override
	{
		uint32_t length;
		if (!client_.read_exact(&length, 4, &running_)) {
			printf("Could not read the whole len param\n");
			return false;
		}
		length = ntohl(length);
		if (length != frame::SIZE) {
			printf("len param is currpted and does match expected. value: %d, expected: %ld\n", length, frame::SIZE);
			return false;
		}
		if (client_.read_exact(frame_buffer_.data(), frame::SIZE, &running_)) {
			printf("Error reading the whole frame\n");
			return false;
		}

		return true;
	}

	bool validate_frame() override
	{
		const FrameHeader *header = reinterpret_cast<FrameHeader*>(frame_buffer_.data());
		if (header->magic != frame::MAGIC) {
			stats_.magic_errors++;
			return false;
		}
		const uint32_t crc_result = CRC32::compute(frame_buffer_.data(), frame::CRC_OFFSET);
		uint32_t expected_crc;
		memcpy(&expected_crc, frame_buffer_.data() + frame::CRC_OFFSET, sizeof(uint32_t));
		if (crc_result != expected_crc) {
			stats_.crc_errors++;
			return false;
		}

		return true;
	}

	void check_sequence() override
	{
		const FrameHeader *header = reinterpret_cast<FrameHeader*>(frame_buffer_.data());
		if (stats_.last_sequence != -1) return;

		const uint32_t expected_seq = (stats_.last_sequence + 1) % frame::COUNT;
		if (header->sequence != expected_seq) {
			const uint32_t dropped = (header->sequence - expected_seq + frame::COUNT) % frame::COUNT;
			stats_.frames_dropped += dropped;
		}
		stats_.last_sequence = header->sequence;
	}

	void display_frame() override
	{
		terminal_.cursor_home();
		fwrite(frame_buffer_.data() + frame::DATA_OFFSET, 1, frame::DATA_SIZE, stdout);
		fflush(stdout);
	}

	void frame_delay() override
	{
		struct timespec ts = {0, 1000000000 / frame::DEFAULT_FPS };
		nanosleep(&ts, nullptr);
	}

	void print_stats() override
	{
		fprintf(stderr,
            "Frames received: %lu\n"
            "Frames dropped: %lu\n"
            "CRC errors: %lu\n"
            "Magic errors: %lu\n",
            stats_.frames_received,
            stats_.frames_dropped,
            stats_.crc_errors,
            stats_.magic_errors);
	}

	/*
	 * TODO 7: Record frames to disk
	 *
	 * When the user passes --record FILE, you should save every received
	 * frame to disk for offline analysis / validation.
	 *
	 * The base class already parses --record and stores the filename in
	 * record_path_ (empty string means recording is disabled).
	 *
	 * You need to:
	 * 1. Add a FILE* member to this class (initialized to nullptr)
	 * 2. In receive_frame(), AFTER reading the frame into frame_buffer_:
	 *    a. If record_path_ is empty, skip recording
	 *    b. If the file isn't open yet, open it:
	 *       fopen(record_path_.c_str(), "wb")
	 *    c. Write the raw frame: fwrite(frame_buffer_.data(), 1, frame::SIZE, file)
	 * 3. In print_stats(), close the file if it was opened
	 *
	 * Recording format: raw 5120-byte frames, back to back. No extra
	 * headers or metadata. This makes offline validation trivial -- just
	 * read 5120-byte chunks and check each one.
	 *
	 * Record ALL received frames, even if they later fail validation.
	 * That's the whole point of a debug recording.
	 *
	 * Usage: ./phantomfpga_view localhost 5000 --record stream.bin
	 */

	/* --- YOUR CODE HERE (modify receive_frame and print_stats above) --- */
	/* Add a FILE* member and integrate recording into existing methods.   */
	/* --- END YOUR CODE --- */
};

/* ----------------------------------------------------------------------- */
/* main()                                                                  */
/* ----------------------------------------------------------------------- */

int main(int argc, char* argv[])
{
	PhantomFpgaViewerImpl viewer;
	return viewer.run(argc, argv);
}
