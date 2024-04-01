#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/ioctl.h>

extern struct MiniRV32IMAState core;
extern void DumpState(struct MiniRV32IMAState *core);
extern void app_main(void);
extern char kernel_start[], kernel_end[], dtb_start[], dtb_end[];

static int ramfd;
static int is_eofd;

static void CtrlC(int sig)
{
	DumpState(&core);
	exit(0);
}

static void ResetKeyboardInput(void)
{
	// Re-enable echo, etc. on keyboard.
	struct termios term;
	tcgetattr(0, &term);
	term.c_lflag |= ICANON | ECHO;
	tcsetattr(0, TCSANOW, &term);
}

// Override keyboard, so we can capture all keyboard input for the VM.
static void CaptureKeyboardInput(void)
{
	struct termios term;

	// Hook exit, because we want to re-enable keyboard.
	atexit(ResetKeyboardInput);
	signal(SIGINT, CtrlC);

	tcgetattr(0, &term);
	term.c_lflag &= ~(ICANON | ECHO); // Disable echo as well
	tcsetattr(0, TCSANOW, &term);
}

uint64_t GetTimeMicroseconds()
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_usec + ((uint64_t)(tv.tv_sec)) * 1000000LL;
}

int ReadKBByte(void)
{
	char rxchar;
	int rread;

	if (is_eofd)
		return 0xffffffff;

	rread = read(fileno(stdin), &rxchar, 1);

	if (rread > 0) // Tricky: getchar can't be used with arrow keys.
		return rxchar;
	else
		return -1;
}

int IsKBHit(void)
{
	int byteswaiting;

	if (is_eofd)
		return -1;

	ioctl(0, FIONREAD, &byteswaiting);
	// Is end-of-file for
	if (!byteswaiting && write(fileno(stdin), 0, 0 ) != 0) {
		is_eofd = 1;
		return -1;
	}
	return !!byteswaiting;
}

int psram_init(void)
{
	ramfd = open("/tmp/ram", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (ramfd < 0) {
		perror("open\n");
		return -1;
	}

	return 0;
}

int psram_read(uint32_t addr, void *buf, int len)
{
	lseek(ramfd, addr, SEEK_SET);
	read(ramfd, buf, len);
}

int psram_write(uint32_t addr, void *buf, int len)
{
	lseek(ramfd, addr, SEEK_SET);
	write(ramfd, buf, len);
}

int load_images(int ram_size, int *kern_len, int *dtb_len)
{
	int dtb_ptr;
	long flen;

	flen = kernel_end - kernel_start;
	if (flen > ram_size) {
		fprintf(stderr, "Error: Could not fit RAM image (%ld bytes) into %d\n", flen, ram_size);
		return -1;
	}
	if (kern_len)
		*kern_len = flen;

	write(ramfd, kernel_start, flen);

	flen = dtb_end - dtb_start;
	if (dtb_len)
		*dtb_len = flen;

	dtb_ptr = ram_size - flen;
	lseek(ramfd, dtb_ptr, SEEK_SET);
	write(ramfd, dtb_start, flen);

	return 0;
}

int main(int argc, char **argv)
{
	CaptureKeyboardInput();
	app_main();
}
