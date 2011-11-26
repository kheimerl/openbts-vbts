/*
* Copyright 2008, 2009, 2010 Free Software Foundation, Inc.
*
* This software is distributed under the terms of the GNU Affero Public License.
* See the COPYING file in the main directory for details.
*
* This use of this software may be subject to additional restrictions.
* See the LEGAL file in the main directory for details.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "radioDevice.h"
#include "Threads.h"
#include "Logger.h"
#include <uhd/usrp/single_usrp.hpp>
#include <uhd/utils/thread_priority.hpp>

/*
    use_ext_ref       - Enable external 10MHz clock reference

    master_clk_rt     - Master clock frequency

    rx_smpl_offset    - Timing correction in seconds between receive and
                        transmit timestamps. This value corrects for delays on
                        on the RF side of the timestamping point of the device.

    smpl_buf_sz       - The receive sample buffer size in bytes. 
*/
const bool use_ext_ref = false;
const double master_clk_rt = 52e6;
const double rx_smpl_offset = .00005;
const size_t smpl_buf_sz = (1 << 20);

/** Timestamp conversion
    @param timestamp a UHD or OpenBTS timestamp
    @param rate sample rate
    @return the converted timestamp
*/
uhd::time_spec_t convert_time(TIMESTAMP ticks, double rate)
{
	double secs = (double) ticks / rate;
	return uhd::time_spec_t(secs);
}

TIMESTAMP convert_time(uhd::time_spec_t ts, double rate)
{
	size_t ticks = ts.get_full_secs() * rate;
	return ts.get_tick_count(rate) + ticks;
}

/*
    Sample Buffer - Allows reading and writing of timed samples using OpenBTS
                    or UHD style timestamps. Time conversions are handled
                    internally or accessable through the static convert calls.
*/
class smpl_buf {
public:
	/** Sample buffer constructor
	    @param len number of 32-bit samples the buffer should hold
	    @param rate sample clockrate 
	    @param timestamp 
	*/
	smpl_buf(size_t len, double rate);
	~smpl_buf();

	/** Query number of samples available for reading
	    @param timestamp time of first sample
	    @return number of available samples or error
	*/
	ssize_t avail_smpls(TIMESTAMP timestamp) const;
	ssize_t avail_smpls(uhd::time_spec_t timestamp) const;

	/** Read and write
	    @param buf pointer to buffer
	    @param len number of samples desired to read or write
	    @param timestamp time of first stample
	    @return number of actual samples read or written or error
	*/
	ssize_t read(void *buf, size_t len, TIMESTAMP timestamp);
	ssize_t read(void *buf, size_t len, uhd::time_spec_t timestamp);
	ssize_t write(void *buf, size_t len, TIMESTAMP timestamp);
	ssize_t write(void *buf, size_t len, uhd::time_spec_t timestamp);

	/** Buffer status string
	    @return a formatted string describing internal buffer state
	*/
	std::string str_status() const;

	/** Formatted error string 
	    @param code an error code
	    @return a formatted error string
	*/
	static std::string str_code(ssize_t code);

	enum err_code {
		ERROR_TIMESTAMP = -1,
		ERROR_READ = -2,
		ERROR_WRITE = -3,
		ERROR_OVERFLOW = -4
	};

private:
	uint32_t *data;
	size_t buf_len;

	double clk_rt;

	TIMESTAMP time_start;
	TIMESTAMP time_end;

	size_t data_start;
	size_t data_end;
};

/*
    uhd_device - UHD implementation of the Device interface. Timestamped samples
                are sent to and received from the device. An intermediate buffer
                on the receive side collects and aligns packets of samples.
                Events and errors such as underruns are reported asynchronously
                by the device and received in a separate thread.
*/
class uhd_device : public RadioDevice {
public:
	uhd_device(double rate, bool skip_rx);
	~uhd_device();

	bool open();
	bool start();
	bool stop();
	void setPriority();

	int readSamples(short *buf, int len, bool *overrun, 
			TIMESTAMP timestamp, bool *underrun, unsigned *RSSI);

	int writeSamples(short *buf, int len, bool *underrun, 
			 TIMESTAMP timestamp, bool isControl);

	bool updateAlignment(TIMESTAMP timestamp);

	bool setTxFreq(double wFreq);
	bool setRxFreq(double wFreq);

	inline TIMESTAMP initialWriteTimestamp() { return 0; }
	inline TIMESTAMP initialReadTimestamp() { return 0; }

	inline double fullScaleInputValue() { return 13500.0; }
	inline double fullScaleOutputValue() { return 9450.0; }

	double setRxGain(double dB) { return 0; }
	double getRxGain(void) { return 0; }
	double maxRxGain(void) { return 0; }
	double minRxGain(void) { return 0; }

	double setTxGain(double dB) { return 0; }
	double maxTxGain(void) { return 0; }
	double minTxGain(void) { return 0; }

	double getTxFreq() { return 0; }
	double getRxFreq() { return 0; }

	inline double getSampleRate() { return actual_smpl_rt; }
	inline double numberRead() { return rx_pkt_cnt; }
	inline double numberWritten() { return 0; }

	/** Receive and process asynchronous message
	    @return true if message received or false on timeout or error
	*/
	bool recv_async_msg();

private:
	uhd::usrp::single_usrp::sptr usrp_dev;

	double desired_smpl_rt;
	double actual_smpl_rt;

	size_t tx_spp;
	size_t rx_spp;

	bool started;
	bool aligned;
	bool skip_rx;

	size_t rx_pkt_cnt;
	size_t drop_cnt;
	uhd::time_spec_t prev_ts;

	TIMESTAMP ts_offset;
	smpl_buf *rx_smpl_buf;

	bool flush_recv(size_t num_pkts);
	std::string str_code(uhd::rx_metadata_t metadata);
	std::string str_code(uhd::async_metadata_t metadata);

	Thread async_event_thrd;
};

void *async_event_loop(uhd_device *dev)
{
	while (1) {
		dev->recv_async_msg();
		pthread_testcancel();
	}
}

uhd_device::uhd_device(double rate, bool skip_rx)
	: desired_smpl_rt(rate), actual_smpl_rt(0), tx_spp(0), rx_spp(0),
	  started(false), aligned(true), rx_pkt_cnt(0), drop_cnt(0),
	  prev_ts(0,0), ts_offset(0), rx_smpl_buf(NULL)
{
	this->skip_rx = skip_rx;
}

uhd_device::~uhd_device()
{
	stop();

	if (rx_smpl_buf)
		delete rx_smpl_buf;
}

static double set_usrp_rates(uhd::usrp::single_usrp::sptr dev, double rate)
{
	double actual_rate;

	dev->set_tx_rate(rate);
	dev->set_rx_rate(rate);
	actual_rate = dev->get_tx_rate();

	if (actual_rate != rate) {
		LOG(ERROR) << "Actual sample rate differs from desired rate";
		return -1.0;
	}
	if (dev->get_rx_rate() != actual_rate) {
		LOG(ERROR) << "Transmit and receive sample rates do not match";
		return -1.0;
	}

	return actual_rate;
}

static void set_usrp_tx_gain(uhd::usrp::single_usrp::sptr dev, double)
{
	uhd::gain_range_t range = dev->get_tx_gain_range();
	dev->set_tx_gain((range.start() + range.stop()) / 2);
}

static void set_usrp_rx_gain(uhd::usrp::single_usrp::sptr dev, double)
{
	uhd::gain_range_t range = dev->get_rx_gain_range();
	dev->set_rx_gain((range.start() + range.stop()) / 2);
}

static void set_usrp_ref_clk(uhd::usrp::single_usrp::sptr dev, bool ext_clk)
{
	uhd::clock_config_t clk_cfg;

	clk_cfg.pps_source = uhd::clock_config_t::PPS_SMA;
	clk_cfg.pps_polarity = uhd::clock_config_t::PPS_NEG;

	if (ext_clk)
		clk_cfg.ref_source = uhd::clock_config_t::REF_SMA;
	else
		clk_cfg.ref_source = uhd::clock_config_t::REF_INT;

	dev->set_clock_config(clk_cfg);
}

bool uhd_device::open()
{
	LOG(INFO) << "creating USRP device...";

	// Use the first available USRP E100
	uhd::device_addr_t dev_addr("type=usrp-e");
	try {
		usrp_dev = uhd::usrp::single_usrp::make(dev_addr);
	}
	
	catch(...) {
		LOG(ERROR) << "USRP make failed";
		return false;
	}

	// Set master clock rate
	usrp_dev->set_master_clock_rate(master_clk_rt);

	// Number of samples per over-the-wire packet
	tx_spp = usrp_dev->get_device()->get_max_send_samps_per_packet();
	rx_spp = usrp_dev->get_device()->get_max_recv_samps_per_packet();

	// Set rates
	actual_smpl_rt = set_usrp_rates(usrp_dev, desired_smpl_rt);
	if (actual_smpl_rt < 0)
		return false;

	// Create receive buffer
	size_t buf_len = smpl_buf_sz / sizeof(uint32_t);
	rx_smpl_buf = new smpl_buf(buf_len, actual_smpl_rt);

	// Set receive chain sample offset 
	ts_offset = (TIMESTAMP)(rx_smpl_offset * actual_smpl_rt);

	// Set gains to midpoint
	set_usrp_tx_gain(usrp_dev, 0.0);

	// Set reference clock
	set_usrp_ref_clk(usrp_dev, use_ext_ref);

	// Print configuration
	LOG(INFO) << usrp_dev->get_pp_string();

	return true;
}

bool uhd_device::flush_recv(size_t num_pkts)
{
	uhd::rx_metadata_t metadata;
	size_t num_smpls;
	uint32_t buff[rx_spp];

	for (size_t i = 0; i < num_pkts; i++) {
		num_smpls = usrp_dev->get_device()->recv(
					buff,
					rx_spp,
					metadata,
					uhd::io_type_t::COMPLEX_INT16,
					uhd::device::RECV_MODE_ONE_PACKET);

		if (!num_smpls)
			return false;
	}

	return true;
}

bool uhd_device::start()
{
	LOG(INFO) << "Starting USRP...";

	if (started) {
		LOG(ERROR) << "Device already started";
		return false;
	}

	setPriority();

	// Start asynchronous event (underrun check) loop
	async_event_thrd.start((void * (*)(void*))async_event_loop, (void*)this);

	// Start streaming
	uhd::stream_cmd_t cmd = uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS;
	cmd.stream_now = true;
	usrp_dev->set_time_now(uhd::time_spec_t(0.0));

	if (!skip_rx)
		usrp_dev->issue_stream_cmd(cmd);

	// Flush out any early garbage
	if (!flush_recv(20))
		return false;

	// Display usrp time
	double time_now = usrp_dev->get_time_now().get_real_secs();
	LOG(INFO) << "The current time is " << time_now << " seconds";

	started = true;
	return true;
}

bool uhd_device::stop()
{
	uhd::stream_cmd_t stream_cmd = 
		uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;

	usrp_dev->issue_stream_cmd(stream_cmd);

	started = false;
	return true;
}

void uhd_device::setPriority()
{
	uhd::set_thread_priority_safe();
	return;
}

static int check_rx_md_err(uhd::rx_metadata_t &md, uhd::time_spec_t &prev_ts)
{
	uhd::time_spec_t ts;

	// Missing timestamp
	if (!md.has_time_spec) {
		LOG(ERROR) << "UHD: Received packet missing timestamp";
		return -1;
	}

	ts = md.time_spec;

	// Monotonicity check
	if (ts < prev_ts) {
		LOG(ERROR) << "UHD: Loss of monotonic: " << ts.get_real_secs();
		LOG(ERROR) << "UHD: Previous time: " << prev_ts.get_real_secs();
		return -1;
	} else {
		prev_ts = ts;
	}

	return 0;
}

int uhd_device::readSamples(short *buf, int len, bool *overrun,
			TIMESTAMP timestamp, bool *underrun, unsigned *RSSI)
{
	ssize_t rc;
	uhd::time_spec_t ts;
	uhd::rx_metadata_t metadata;
	uint32_t pkt_buf[rx_spp];

	if (skip_rx)
		return 0;

	// Shift read time with respect to transmit clock
	timestamp += ts_offset;

	ts = convert_time(timestamp, actual_smpl_rt);
	LOG(DEEPDEBUG) << "Requested timestamp = " << ts.get_real_secs();

	// Check that timestamp is valid
	rc = rx_smpl_buf->avail_smpls(timestamp);
	if (rc < 0) {
		LOG(ERROR) << rx_smpl_buf->str_code(rc);
		LOG(ERROR) << rx_smpl_buf->str_status();
		return 0;
	}

	// Receive samples from the usrp until we have enough
	while (rx_smpl_buf->avail_smpls(timestamp) < len) {
		size_t num_smpls = usrp_dev->get_device()->recv(
					(void*)pkt_buf,
					rx_spp,
					metadata,
					uhd::io_type_t::COMPLEX_INT16,
					uhd::device::RECV_MODE_ONE_PACKET);

		rx_pkt_cnt++;

		// Recv error in UHD
		if (!num_smpls) {
			LOG(ERROR) << str_code(metadata);
			return 0;
		}

		// Other metadata timing checks
		if (check_rx_md_err(metadata, prev_ts) < 0)
			return 0;

		ts = metadata.time_spec;
		LOG(DEEPDEBUG) << "Received timestamp = " << ts.get_real_secs();

		rc = rx_smpl_buf->write(pkt_buf,
					num_smpls,
					metadata.time_spec);

		// Continue on local overrun, exit on other errors
		if ((rc < 0)) {
			LOG(ERROR) << rx_smpl_buf->str_code(rc);
			LOG(ERROR) << rx_smpl_buf->str_status();
			if (rc != smpl_buf::ERROR_OVERFLOW)
				return 0;
		}
	}

	// We have enough samples
	rc = rx_smpl_buf->read(buf, len, timestamp);
	if ((rc < 0) || (rc != len)) {
		LOG(ERROR) << rx_smpl_buf->str_code(rc);
		LOG(ERROR) << rx_smpl_buf->str_status();
		return 0;
	}

	return len;
}

int uhd_device::writeSamples(short *buf, int len, bool *underrun,
			unsigned long long timestamp,bool isControl)
{
	uhd::tx_metadata_t metadata;
	metadata.has_time_spec = true;
	metadata.start_of_burst = false;
	metadata.end_of_burst = false;
	metadata.time_spec = convert_time(timestamp, actual_smpl_rt);

	// No control packets
	if (isControl) {
		LOG(ERROR) << "Control packets not supported";
		return 0;
	}

	// Drop a fixed number of packets (magic value)
	if (!aligned) {
		drop_cnt++;

		if (drop_cnt == 1) {
			LOG(DEBUG) << "Aligning transmitter: stop burst";
			metadata.end_of_burst = true;
		} else if (drop_cnt < 30) {
			LOG(DEEPDEBUG) << "Aligning transmitter: packet advance";
			*underrun = true;
			return len;
		} else {
			LOG(DEBUG) << "Aligning transmitter: start burst";
			metadata.start_of_burst = true;
			aligned = true;
			drop_cnt = 0;
		}
	}

	size_t num_smpls = usrp_dev->get_device()->send(buf,
					len,
					metadata,
					uhd::io_type_t::COMPLEX_INT16,
					uhd::device::SEND_MODE_FULL_BUFF);

	if (num_smpls != (unsigned)len)
		LOG(ERROR) << "UHD: Sent fewer samples than requested";

	return num_smpls;
}

bool uhd_device::updateAlignment(TIMESTAMP timestamp)
{
	/* NOP */
	return true;
}

bool uhd_device::setTxFreq(double wFreq)
{
	uhd::tune_result_t tr = usrp_dev->set_tx_freq(wFreq);
	LOG(INFO) << tr.to_pp_string();
	return true;
}

bool uhd_device::setRxFreq(double wFreq)
{
	uhd::tune_result_t tr = usrp_dev->set_rx_freq(wFreq);
	LOG(INFO) << tr.to_pp_string();
	return true;
}

bool uhd_device::recv_async_msg()
{
	uhd::async_metadata_t metadata;
	if (!usrp_dev->get_device()->recv_async_msg(metadata))
		return false;

	// Assume that any error requires resynchronization
	if (metadata.event_code != uhd::async_metadata_t::EVENT_CODE_BURST_ACK) {
		aligned = false;
		LOG(INFO) << str_code(metadata);
	}

	return true;
}

std::string uhd_device::str_code(uhd::rx_metadata_t metadata)
{
	std::ostringstream ost("UHD: ");

	switch (metadata.error_code) {
	case uhd::rx_metadata_t::ERROR_CODE_NONE:
		ost << "No error";
		break;
	case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
		ost << "No packet received, implementation timed-out";
		break;
	case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
		ost << "A stream command was issued in the past";
		break;
	case uhd::rx_metadata_t::ERROR_CODE_BROKEN_CHAIN:
		ost << "Expected another stream command";
		break;
	case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
		ost << "An internal receive buffer has filled";
		break;
	case uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET:
		ost << "The packet could not be parsed";
		break;
	default:
		ost << "Unknown error " << metadata.error_code;
	}

	if (metadata.has_time_spec)
		ost << " at " << metadata.time_spec.get_real_secs() << " sec.";

	return ost.str();
}

std::string uhd_device::str_code(uhd::async_metadata_t metadata)
{
	std::ostringstream ost("UHD: ");

	switch (metadata.event_code) {
	case uhd::async_metadata_t::EVENT_CODE_BURST_ACK:
		ost << "A packet was successfully transmitted";
		break;
	case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
		ost << "An internal send buffer has emptied";
		break;
	case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR:
		ost << "Packet loss between host and device";
		break;
	case uhd::async_metadata_t::EVENT_CODE_TIME_ERROR:
		ost << "Packet time was too late or too early";
		break;
	case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
		ost << "Underflow occurred inside a packet";
		break;
	case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR_IN_BURST:
		ost << "Packet loss within a burst";
		break;
	default:
		ost << "Unknown error " << metadata.event_code;
	}

	if (metadata.has_time_spec)
		ost << " at " << metadata.time_spec.get_real_secs() << " sec.";

	return ost.str();
}

smpl_buf::smpl_buf(size_t len, double rate)
	: buf_len(len), clk_rt(rate),
	  time_start(0), time_end(0), data_start(0), data_end(0)
{
	data = new uint32_t[len];
}

smpl_buf::~smpl_buf()
{
	delete[] data;
}

ssize_t smpl_buf::avail_smpls(TIMESTAMP timestamp) const
{
	if (timestamp < time_start)
		return ERROR_TIMESTAMP;
	else if (timestamp >= time_end)
		return 0;
	else
		return time_end - timestamp;
}

ssize_t smpl_buf::avail_smpls(uhd::time_spec_t timespec) const
{
	return avail_smpls(convert_time(timespec, clk_rt));
}

ssize_t smpl_buf::read(void *buf, size_t len, TIMESTAMP timestamp)
{
	// Check for valid read
	if (timestamp < time_start)
		return ERROR_TIMESTAMP;
	if (timestamp >= time_end)
		return 0;
	if (len >= buf_len)
		return ERROR_READ;

	// How many samples should be copied
	size_t num_smpls = time_end - timestamp;
	if (num_smpls > len);
		num_smpls = len;

	// Starting index
	size_t read_start = data_start + (timestamp - time_start);

	// Read it
	if (read_start + num_smpls < buf_len) {
		size_t numBytes = len * 2 * sizeof(short);
		memcpy(buf, data + read_start, numBytes);
	} else {
		size_t first_cp = (buf_len - read_start) * 2 * sizeof(short);
		size_t second_cp = len * 2 * sizeof(short) - first_cp;

		memcpy(buf, data + read_start, first_cp);
		memcpy((char*) buf + first_cp, data, second_cp);
	}

	data_start = (read_start + len) % buf_len;
	time_start = timestamp + len;

	if (time_start > time_end)
		return ERROR_READ;
	else
		return num_smpls;
}

ssize_t smpl_buf::read(void *buf, size_t len, uhd::time_spec_t ts)
{
	return read(buf, len, convert_time(ts, clk_rt));
}

ssize_t smpl_buf::write(void *buf, size_t len, TIMESTAMP timestamp)
{
	// Check for valid write
	if ((len == 0) || (len >= buf_len))
		return ERROR_WRITE;
	if ((timestamp + len) <= time_end)
		return ERROR_TIMESTAMP;

	// Starting index
	size_t write_start = (data_start + (timestamp - time_start)) % buf_len;

	// Write it
	if ((write_start + len) < buf_len) {
		size_t numBytes = len * 2 * sizeof(short);
		memcpy(data + write_start, buf, numBytes);
	} else {
		size_t first_cp = (buf_len - write_start) * 2 * sizeof(short);
		size_t second_cp = len * 2 * sizeof(short) - first_cp;

		memcpy(data + write_start, buf, first_cp);
		memcpy(data, (char*) buf + first_cp, second_cp);
	}

	data_end = (write_start + len) % buf_len;
	time_end = timestamp + len;

	if (((write_start + len) > buf_len) && (data_end > data_start))
		return ERROR_OVERFLOW;
	else if (time_end <= time_start)
		return ERROR_WRITE;
	else
		return len;
}

ssize_t smpl_buf::write(void *buf, size_t len, uhd::time_spec_t ts)
{
	return write(buf, len, convert_time(ts, clk_rt));
}

std::string smpl_buf::str_status() const
{
	std::ostringstream ost("Sample buffer: ");

	ost << "length = " << buf_len;
	ost << ", time_start = " << time_start;
	ost << ", time_end = " << time_end;
	ost << ", data_start = " << data_start;
	ost << ", data_end = " << data_end;

	return ost.str();
}

std::string smpl_buf::str_code(ssize_t code)
{
	switch (code) {
	case ERROR_TIMESTAMP:
		return "Sample buffer: Requested timestamp is not valid";
	case ERROR_READ:
		return "Sample buffer: Read error";
	case ERROR_WRITE:
		return "Sample buffer: Write error";
	case ERROR_OVERFLOW:
		return "Sample buffer: Overrun";
	default:
		return "Sample buffer: Unknown error";
	}
}

RadioDevice *RadioDevice::make(double smpl_rt, bool skip_rx)
{
	return new uhd_device(smpl_rt, skip_rx);
}
