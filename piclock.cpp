// OpenVG Clock
// Simon Hyde <simon.hyde@bbc.co.uk>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <string>
#include <fstream>
#include <map>
#include <queue>
#include <cctype>
#include <memory>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <boost/program_options.hpp>
#include <netdb.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <Magick++.h>
#include <nanovg.h>
#include <fcntl.h>
#include "ntpstat/ntpstat.h"
#include "piclock_messages.h"
#include "nvg_main.h"
#include "globals.h"
#include "control_tcp.h"
#include "nvg_helpers.h"
#include "fonts.h"
#include "nvg_helpers.h"
#include "tally.h"
#include "countdownclock.h"
#include "regionstate.h"
#include "imagescaling.h"
#include "overallstate.h"
#include "gpio.h"

#define FPS 25
#define FRAMES 0

namespace po = boost::program_options;

static OverallState globalState;

int GPI_MODE = 0;
int GPIO_TYPE = 0;
std::string GPIO_PULLS;
int init_window_width = 0;
int init_window_height = 0;
std::string clean_exit_file("/tmp/piclock_clean_exit");

void DrawFrame(NVGcontext *vg, int iwidth, int iheight);
void NvgInit(NVGcontext *vg);

void *ntp_check_thread(void *arg)
{
	ntpstate_t *data = (ntpstate_t *)arg;
	while (bRunning)
	{
		get_ntp_state(data);
		sleep(1);
	}
	return NULL;
}

void read_settings(const std::string &filename, po::variables_map &vm)
{
	po::options_description desc("Options");
	desc.add_options()("init_window_width", po::value<int>(&init_window_width)->default_value(0), "Initial window width, specifying 0 gives fullscreen mode")("init_window_height", po::value<int>(&init_window_height)->default_value(0), "Initial window height, specifying 0 gives fullscreen mode")("gpio_mode", po::value<int>(&GPIO_TYPE)->default_value(0), "GPIO Type, 0=PiFace Digital, 1=Raspberry Pi (not yet implemented)")("gpio_pulls", po::value<std::string>(&GPIO_PULLS)->default_value("UUUUUUUU"), "GPI Pull Up/Down/Off status")("tally_mode", po::value<int>(&GPI_MODE)->default_value(0), "Tally Mode, 0=disabled, 1=GPI/O, 2=TCP/IP, 3=TCP/IP with GPIO status passed back to controller")("tally_remote_host", po::value<std::vector<std::string>>(&tally_hosts), "Remote tally host, may be specified multiple times for multiple connections")("tally_remote_port", po::value<std::string>(&TALLY_SERVICE)->default_value("6254"), "Port (or service) to connect to on (default 6254)")("tally_shared_secret", po::value<std::string>(&TALLY_SECRET)->default_value("SharedSecretGoesHere"), "Shared Secret (password) for connecting to tally service")("clean_exit_file", po::value<std::string>(&clean_exit_file)->default_value("/tmp/piclock_clean_exit"), "Flag file created to indicate a clean exit (from keyboard request)");

	std::ifstream settings_file(filename.c_str());
	vm = po::variables_map();
	po::store(po::parse_config_file(settings_file, desc), vm);
	po::notify(vm);
}

void cleanup()
{
	bRunning = false;
	ResizeQueue::Abort();

	if (!clean_exit_file.empty())
		open(clean_exit_file.c_str(), O_CREAT, 00666);
	exit(0);
}

void KeyCallback(unsigned char key, int x, int y)
{
	if (key == 'q')
	{
		cleanup();
	}
}

static struct timeval tval;
static ntpstate_t ntp_state_data;

int main(int argc, char *argv[])
{
	po::variables_map vm;

	std::string configFile = "/etc/piclock.cfg";

	if (argc > 1)
		configFile = argv[1];
	read_settings(configFile, vm);

	gettimeofday(&tval, NULL);

	init_ntp_state();
	get_ntp_state(&ntp_state_data);
	pthread_t ntp_thread;
	pthread_attr_t ntp_attr;
	pthread_attr_init(&ntp_attr);
	pthread_create(&ntp_thread, &ntp_attr, &ntp_check_thread, &ntp_state_data);
	std::thread resize_thread(ResizeQueue::RunBackgroundResizeThread, std::ref(bRunning));
	struct sched_param resize_param;
	resize_param.sched_priority = sched_get_priority_min(SCHED_IDLE);
	pthread_setschedparam(resize_thread.native_handle(), SCHED_IDLE, &resize_param);
	if (GPI_MODE & 1)
		gpio_init(GPIO_TYPE, GPIO_PULLS);
	if (GPI_MODE & 2)
		create_tcp_threads();

	nvg_main(DrawFrame, NvgInit, init_window_width, init_window_height);

	cleanup();
}

void NvgInit(NVGcontext *vg)
{
	globalState.NvgInit(vg);
}

void DrawFrame(NVGcontext *vg, int iwidth, int iheight)
{
    static struct tm tm_local;

    // Update the current time
    gettimeofday(&tval, NULL);
    localtime_r(&tval.tv_sec, &tm_local);

    // Set the background color
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, iwidth, iheight);
    nvgFillColor(vg, nvgRGB(0, 0, 0)); // Black background
    nvgFill(vg);

    // Set the text color and font for the day and date
    nvgFontFace(vg, globalState.FontDate().c_str());
    nvgFillColor(vg, nvgRGB(255, 255, 255)); // White text

    // Format the day and date string (no comma between day and month)
    char dateStr[64];
    strftime(dateStr, sizeof(dateStr), "%a %b %d", &tm_local); // No comma

    // Determine font size for the day and date
    float dateFontSize = iheight * 0.165; // 10% larger
    nvgFontSize(vg, dateFontSize);

    // Measure text bounds for the day and date
    float dateBounds[4];
    nvgTextBounds(vg, 0, 0, dateStr, NULL, dateBounds);

    float dateTextWidth = dateBounds[2] - dateBounds[0];
    float dateTextHeight = dateBounds[3] - dateBounds[1];
    float dateX = (iwidth - dateTextWidth) / 2.0f;

    // Position the date higher on the screen (top 1/3rd)
    float dateY = (iheight / 6.0f) + (dateTextHeight / 2.0f);

    // Render the day and date
    nvgText(vg, dateX, dateY, dateStr, NULL);

    // Set the text color and font for the time
    nvgFontFace(vg, globalState.FontDigital().c_str());
    nvgFillColor(vg, nvgRGB(255, 255, 255)); // White text

    // Format the local time string (12-hour format with AM/PM)
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%I:%M %p", &tm_local); // 12-hour format

    // Determine font size for the time (reduced by 10%)
    float timeFontSize = iheight * 0.315; // Reduced from 0.35 to 0.315
    nvgFontSize(vg, timeFontSize);

    // Measure text bounds for the time
    float timeBounds[4];
    nvgTextBounds(vg, 0, 0, timeStr, NULL, timeBounds);

    float timeTextWidth = timeBounds[2] - timeBounds[0];
    float timeTextHeight = timeBounds[3] - timeBounds[1];
    float timeX = (iwidth - timeTextWidth) / 2.0f;

    // Move the time significantly lower (bottom 1/3rd)
    float timeY = (iheight / 2.0f) + (iheight / 4.0f);

    // Apply a vertical scaling transformation for taller text
    nvgSave(vg);                    // Save the current transformation state
    nvgTranslate(vg, timeX, timeY); // Move the origin to the text's position
    nvgScale(vg, 1.0, 1.5);         // Scale the text vertically by 1.5 times

    // Render the time
    nvgText(vg, 0, 0, timeStr, NULL);

    // Restore the original transformation state
    nvgRestore(vg);
}
