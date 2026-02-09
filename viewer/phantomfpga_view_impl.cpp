/* SPDX-License-Identifier: MIT */
/*
 * PhantomFPGA Viewer - YOUR IMPLEMENTATION
 *
 * This is the file you need to edit. Implement the TODO methods below
 * to receive, validate, and display ASCII animation frames.
 *
 * Read phantomfpga_view.h for the class interface and frame constants.
 *
 * Available members from the base class (PhantomFpgaViewer):
 *   client_        -- TcpClient with read_exact(buf, len, &running_)
 *   terminal_      -- Terminal with clear_screen(), cursor_home(), etc.
 *   frame_buffer_  -- std::array<uint8_t, 5120> for the current frame
 *   stats_         -- ViewerStats (frames_received, frames_dropped, etc.)
 *   running_       -- volatile bool, goes false on Ctrl+C
 *
 * Frame layout (frame::SIZE = 5120 bytes):
 *   Offset 0:    FrameHeader (16 bytes) -- magic, sequence, reserved
 *   Offset 16:   ASCII data (4995 bytes) -- the actual animation
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
/* Implement the 6 TODO methods below. The base class handles TCP          */
/* connection, terminal setup, signal handling, and the main loop.         */
/* ----------------------------------------------------------------------- */

class PhantomFpgaViewerImpl : public PhantomFpgaViewer {
protected:

	/*
	 * TODO 1: Receive one frame from the server
	 *
	 * The wire protocol sends: 4-byte length (network order) + frame data.
	 *
	 * Steps:
	 * 1. Read 4 bytes into a uint32_t using client_.read_exact()
	 * 2. Convert from network byte order: ntohl()
	 * 3. Verify the length == frame::SIZE (5120)
	 * 4. Read frame::SIZE bytes into frame_buffer_.data()
	 *
	 * Returns true on success, false on error or disconnect.
	 *
	 * Hint: client_.read_exact(buf, len, &running_) handles partial
	 * reads and EINTR for you.
	 */
	bool receive_frame() override
	{
		/* --- YOUR CODE HERE --- */
		fprintf(stderr, "TODO: Implement receive_frame()\n");
		return false;
		/* --- END YOUR CODE --- */
	}

	/*
	 * TODO 2: Validate the frame
	 *
	 * Check two things:
	 * 1. Magic number: cast frame_buffer_.data() to a FrameHeader* and
	 *    check that hdr->magic == frame::MAGIC
	 *    Increment stats_.magic_errors on failure.
	 *
	 * 2. CRC32: compute CRC32::compute(frame_buffer_.data(), frame::CRC_OFFSET)
	 *    Compare with the 4-byte CRC stored at frame::CRC_OFFSET
	 *    (read it as a uint32_t from frame_buffer_[frame::CRC_OFFSET])
	 *    Increment stats_.crc_errors on mismatch.
	 *
	 * Returns true if valid, false otherwise.
	 */
	bool validate_frame() override
	{
		/* --- YOUR CODE HERE --- */
		fprintf(stderr, "TODO: Implement validate_frame()\n");
		return false;
		/* --- END YOUR CODE --- */
	}

	/*
	 * TODO 3: Check sequence continuity
	 *
	 * Detect dropped frames by looking at sequence number gaps.
	 *
	 * Steps:
	 * 1. Get the sequence number from the FrameHeader
	 * 2. If this isn't the first frame (stats_.last_sequence != -1):
	 *    a. Calculate expected = (stats_.last_sequence + 1) % frame::COUNT
	 *    b. If current != expected:
	 *       dropped = (current - expected + frame::COUNT) % frame::COUNT
	 *       stats_.frames_dropped += dropped
	 * 3. Update stats_.last_sequence
	 */
	void check_sequence() override
	{
		/* --- YOUR CODE HERE --- */
		fprintf(stderr, "TODO: Implement check_sequence()\n");
		/* --- END YOUR CODE --- */
	}

	/*
	 * TODO 4: Display the frame
	 *
	 * The ASCII frame data starts at frame::DATA_OFFSET (16 bytes in)
	 * and is frame::DATA_SIZE (4995) bytes long. It already contains
	 * newlines separating the rows -- just dump it to stdout.
	 *
	 * Steps:
	 * 1. Move cursor to top-left: terminal_.cursor_home()
	 * 2. Write the frame data: fwrite() from frame_buffer_ + DATA_OFFSET
	 * 3. Flush stdout: fflush(stdout)
	 */
	void display_frame() override
	{
		/* --- YOUR CODE HERE --- */
		fprintf(stderr, "TODO: Implement display_frame()\n");
		/* --- END YOUR CODE --- */
	}

	/*
	 * TODO 5: Frame rate delay
	 *
	 * Sleep for 1/fps seconds to maintain the target frame rate.
	 *
	 * Steps:
	 * 1. Calculate delay: 1000000000 / frame::DEFAULT_FPS nanoseconds
	 * 2. Use nanosleep() with a struct timespec
	 *
	 * Example:
	 *   struct timespec ts = { 0, 1000000000 / frame::DEFAULT_FPS };
	 *   nanosleep(&ts, nullptr);
	 */
	void frame_delay() override
	{
		/* --- YOUR CODE HERE --- */
		usleep(1000000 / frame::DEFAULT_FPS); /* placeholder */
		/* --- END YOUR CODE --- */
	}

	/*
	 * TODO 6: Print statistics
	 *
	 * Print a summary of what happened. Include:
	 * - stats_.frames_received
	 * - stats_.frames_dropped
	 * - stats_.crc_errors
	 * - stats_.magic_errors
	 *
	 * Use fprintf(stderr, ...) so it doesn't interfere with the animation.
	 */
	void print_stats() override
	{
		/* --- YOUR CODE HERE --- */
		fprintf(stderr, "TODO: Implement print_stats()\n");
		/* --- END YOUR CODE --- */
	}
};

/* ----------------------------------------------------------------------- */
/* main()                                                                  */
/* ----------------------------------------------------------------------- */

int main(int argc, char* argv[])
{
	PhantomFpgaViewerImpl viewer;
	return viewer.run(argc, argv);
}
