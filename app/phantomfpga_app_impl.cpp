/* SPDX-License-Identifier: MIT */
/*
 * PhantomFPGA Application - YOUR IMPLEMENTATION
 *
 * This is the file you need to edit. Implement the TODO methods below
 * to make the app receive frames from the kernel driver and stream
 * them over TCP to the viewer.
 *
 * Read phantomfpga_app.h for the class interface and available members.
 * Read phantomfpga_uapi.h for ioctl definitions and data structures.
 *
 * Available members from the base class (PhantomFpgaApp):
 *   dev_fd_       -- FileDescriptor for the device (you set this in open_device)
 *   buffer_pool_  -- MappedMemory for the DMA buffers (you set this in setup_mmap)
 *   config_       -- AppConfig with parsed CLI parameters
 *   stats_        -- AppStats for your counters
 *   tcp_server_   -- TcpServer (may be nullptr if --tcp-server wasn't used)
 *   running_      -- volatile bool, goes false on Ctrl+C
 *
 * Utility classes:
 *   CRC32::compute(data, len)  -- IEEE 802.3 CRC32, returns uint32_t
 *   FileDescriptor(fd)         -- RAII file descriptor, use std::move()
 *   MappedMemory(addr, size)   -- RAII mmap wrapper, use std::move()
 */

#include "phantomfpga_app.h"

#include <cerrno>
#include <poll.h>
#include <sys/ioctl.h>

/* ----------------------------------------------------------------------- */
/* PhantomFpgaAppImpl -- YOUR CODE GOES HERE                               */
/*                                                                         */
/* Implement all the TODO methods below to make the app work.              */
/* The base class handles everything else (CLI, TCP, signals, cleanup).    */
/* ----------------------------------------------------------------------- */

class PhantomFpgaAppImpl : public PhantomFpgaApp {
protected:

	int open_device() override
	{
		int fd = ::open("/dev/phantomfpga0", O_RDWR);
		if (fd < 0) {
			return -errno;
		}
		this->dev_fd_ = FileDescriptor(fd);
		return 0;
	}

	int configure_device() override
	{
		phantomfpga_config config = {};
		config.desc_count = this->config_.desc_count;
		config.frame_rate = this->config_.frame_rate;
		config.irq_coalesce_count = DEFAULT_IRQ_COUNT;
		config.irq_coalesce_timeout = DEFAULT_IRQ_TIMEOUT;

		int ret = ::ioctl(this->dev_fd_.get(), PHANTOMFPGA_IOCTL_SET_CFG, &config);
		if (ret < 0) return -errno;

		return 0;
	}

	/*
	 * TODO: Set up memory-mapped DMA buffers
	 *
	 * Steps:
	 * 1. Create a struct phantomfpga_buffer_info (zero-init)
	 * 2. Call ioctl(dev_fd_.get(), PHANTOMFPGA_IOCTL_GET_BUFFER_INFO, &info)
	 * 3. Store info.buffer_size in config_.buffer_size
	 * 4. Call mmap():
	 *      void* addr = mmap(nullptr, info.total_size, PROT_READ,
	 *                        MAP_SHARED, dev_fd_.get(), 0);
	 * 5. Store the result: buffer_pool_ = MappedMemory(addr, info.total_size);
	 *
	 * After this, buffer_pool_.get() points to the DMA buffer pool.
	 * Frame N starts at: (uint8_t*)buffer_pool_.get() + N * config_.buffer_size
	 */
	int setup_mmap() override
	{
		/* --- YOUR CODE HERE --- */
		fprintf(stderr, "TODO: Implement setup_mmap()\n");
		return -1;
		/* --- END YOUR CODE --- */
	}

	int start_streaming() override
	{
		int ret = ::ioctl(this->dev_fd_.get(), PHANTOMFPGA_IOCTL_START);
		if (ret < 0) return -errno;
		return 0;
	}

	int stop_streaming() override
	{
		int ret = ::ioctl(this->dev_fd_.get(), PHANTOMFPGA_IOCTL_STOP);
		if (ret < 0) return -errno;
		return 0;
	}

	void main_loop() override
	{
		while (running_) {
			if (tcp_server_)
				tcp_server_->try_accept();

			uint8_t frame_buf[PHANTOMFPGA_FRAME_SIZE];
			struct pollfd pfd = {
				.fd = dev_fd_.get(),
				.events = POLLIN,
				.revents = 0
			};
			int poll_res = poll(&pfd, 1, 100);
			if (0 == poll_res) {
				// poll timeout
				continue;
			}
			if (0 > poll_res) {
				printf("Error polling: %s\n", strerror(errno));
				continue;
			}
			ssize_t bytes_read = read(dev_fd_.get(), frame_buf, PHANTOMFPGA_FRAME_SIZE);
			if (0 > bytes_read) {
				printf("Error reading frame: %d: %s\n", errno, strerror(errno));
				continue;
			}
			if (bytes_read != PHANTOMFPGA_FRAME_SIZE) {
				printf("Error reading frame, frame is too short, size: %ld\n", bytes_read);
				continue;
			}
			printf("Read some frame\n");
			process_frame(frame_buf, bytes_read);
		}
	}

	int process_frame(const void* buffer, uint32_t len) override
	{
		bool valid = validate_frame(buffer, len);
		stats_.frames_received++;
		if (valid) {
			stats_.frames_valid++;
		}

		if (tcp_server_) {
			tcp_server_->send_frame(buffer, len);
		}

		if (config_.verbose) {
			printf("seq: %d, size: %d, valid: %d", stats_.last_seq, len, valid);
		}

		return 0;
	}

	bool validate_frame(const void* frame, uint32_t frame_size) override
	{
		bool ret = true;
		const phantomfpga_frame_header *header = reinterpret_cast<const phantomfpga_frame_header*>(frame);

		if (PHANTOMFPGA_FRAME_MAGIC != header->magic) {
			stats_.magic_errors++;
			ret = false;
		}
		if (stats_.seq_initialized && header->sequence != (stats_.last_seq + 1) % PHANTOMFPGA_FRAME_COUNT) {
			stats_.seq_errors++;
			ret = false;
		}
		if (config_.validate_crc) {
			const uint32_t crc = CRC32::compute(frame, frame_size - 4);
			if (crc != *reinterpret_cast<const uint32_t*>((uint8_t*)frame + frame_size - 4)) {
				stats_.crc_errors++;
				ret = false;
			}
		}

		stats_.last_seq = header->sequence;
		stats_.seq_initialized = true;
		return ret;
	}

	void print_statistics() override
	{
		/* App-side stats */
		fprintf(stderr, "\n=== Application Statistics ===\n");
		fprintf(stderr, "Frames received: %lu\n", stats_.frames_received);
		fprintf(stderr, "Frames valid: %lu\n", stats_.frames_valid);
		fprintf(stderr, "Sequence errors: %lu\n", stats_.seq_errors);
		fprintf(stderr, "Magic errors: %lu\n", stats_.magic_errors);
		fprintf(stderr, "CRC errors: %lu\n", stats_.crc_errors);

		/* Network stats if TCP server is running */
		if (tcp_server_) {
			fprintf(stderr, "\n=== Network Statistics ===\n");
			fprintf(stderr, "Frames sent: %lu\n", tcp_server_->stats().frames_sent);
			fprintf(stderr, "Bytes sent: %lu\n", tcp_server_->stats().bytes_sent);
		}

		/* Device stats */
		if (dev_fd_.valid()) {
			struct phantomfpga_stats dev_stats = {};
			int ret = ioctl(dev_fd_.get(), PHANTOMFPGA_IOCTL_GET_STATS, &dev_stats);
			if (ret == 0) {
				fprintf(stderr, "\n=== Device Statistics ===\n");
				fprintf(stderr, "Frames produced: %llu\n", dev_stats.frames_produced);
				fprintf(stderr, "Frames dropped: %llu\n", dev_stats.frames_dropped);
				fprintf(stderr, "Current frame: %u\n", dev_stats.current_frame);
			}
		}

		/* Runtime duration */
		fprintf(stderr, "\n=== Runtime ===\n");
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		
		uint64_t elapsed_sec = now.tv_sec - stats_.start_time.tv_sec;
		int64_t elapsed_nsec = now.tv_nsec - stats_.start_time.tv_nsec;
		if (elapsed_nsec < 0) {
			elapsed_sec--;
			elapsed_nsec += 1000000000;
		}
		
		fprintf(stderr, "Runtime: %lu.%03lu seconds\n", elapsed_sec, elapsed_nsec / 1000000);
	}
};

/* ----------------------------------------------------------------------- */
/* main()                                                                  */
/* ----------------------------------------------------------------------- */

int main(int argc, char* argv[])
{
	PhantomFpgaAppImpl app;
	return app.run(argc, argv);
}
