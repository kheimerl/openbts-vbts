/*
 * Radio device interface with sample rate conversion
 * Written by Thomas Tsou <ttsou@vt.edu>
 *
 * Copyright 2011 Free Software Foundation, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * See the COPYING file in the main directory for details.
 */

#include <radioInterface.h>
#include <Logger.h>
#include "PAController.h"

/* New chunk sizes for resampled rate */
#ifdef INCHUNK
  #undef INCHUNK
#endif
#ifdef OUTCHUNK
  #undef OUTCHUNK
#endif

/* Resampling parameters */
#define INRATE       65 * SAMPSPERSYM
#define INHISTORY    INRATE * 2
#define INCHUNK      INRATE * 9

#define OUTRATE      96 * SAMPSPERSYM
#define OUTHISTORY   OUTRATE * 2
#define OUTCHUNK     OUTRATE * 9

/* Resampler low pass filters */
signalVector *tx_lpf = 0;
signalVector *rx_lpf = 0;

/* Resampler history */
signalVector *tx_hist = 0;
signalVector *rx_hist = 0;

/* Resampler input buffer */
signalVector *tx_vec = 0;
signalVector *rx_vec = 0;

/*
 * High rate (device facing) buffers
 *
 * Transmit side samples are pushed after each burst so accomodate
 * a resampled burst plus up to a chunk left over from the previous
 * resampling operation.
 *
 * Receive side samples always pulled with a fixed size.
 */
short tx_buf[INCHUNK * 2 * 4];
short rx_buf[OUTCHUNK * 2 * 2];

/* 
 * Utilities and Conversions 
 *
 * Manipulate signal vectors dynamically for two reasons. For one,
 * it's simpler. And two, it doesn't make any reasonable difference
 * relative to the high overhead generated by the resampling.
 */

/* Concatenate signal vectors. Deallocate input vectors. */
signalVector *concat(signalVector *a, signalVector *b)
{
	signalVector *vec = new signalVector(*a, *b);
	delete a;
	delete b;

	return vec;
}

/* Segment a signal vector. Deallocate the input vector. */
signalVector *segment(signalVector *a, int indx, int sz)
{
	signalVector *vec = new signalVector(sz);
	a->segmentCopyTo(*vec, indx, sz);
	delete a;

	return vec;
}

/* Create a new signal vector from a short array. */
signalVector *short_to_sigvec(short *smpls, size_t sz)
{
	int i;
	signalVector *vec = new signalVector(sz);
	signalVector::iterator itr = vec->begin();

	for (i = 0; i < sz; i++) {
		*itr++ = Complex<float>(smpls[2 * i + 0], smpls[2 * i + 1]);
	}

	return vec;
}

/* Convert and deallocate a signal vector into a short array. */
int sigvec_to_short(signalVector *vec, short *smpls)
{
	int i;
	signalVector::iterator itr = vec->begin();

	for (i = 0; i < vec->size(); i++) {
		smpls[2 * i + 0] = itr->real();
		smpls[2 * i + 1] = itr->imag();
		itr++;
	}
	delete vec;

	return i;
}

/* Create a new signal vector from a float array. */
signalVector *float_to_sigvec(float *smpls, int sz)
{
	int i;
	signalVector *vec = new signalVector(sz);
	signalVector::iterator itr = vec->begin();

	for (i = 0; i < sz; i++) {
		*itr++ = Complex<float>(smpls[2 * i + 0], smpls[2 * i + 1]);
	}

	return vec;
}

/* Convert and deallocate a signal vector into a float array. */
int sigvec_to_float(signalVector *vec, float *smpls)
{
	int i;
	signalVector::iterator itr = vec->begin();

	for (i = 0; i < vec->size(); i++) {
		smpls[2 * i + 0] = itr->real();
		smpls[2 * i + 1] = itr->imag();
		itr++;
	}
	delete vec;

	return i;
}

/* Initialize resampling signal vectors */
void init_resampler(signalVector **lpf,
	   	    signalVector **buf,
		    signalVector **hist,
		    int tx)
{
	int P, Q, taps, hist_len;
	float cutoff_freq;

	if (tx) {
		LOG(INFO) << "Initializing Tx resampler";
		P = OUTRATE;
		Q = INRATE;
		taps = 651;
		hist_len = INHISTORY;
	} else {
		LOG(INFO) << "Initializing Rx resampler";
		P = INRATE;
		Q = OUTRATE;
		taps = 961;
		hist_len = OUTHISTORY;
	}

	if (!*lpf) {
		cutoff_freq = (P < Q) ? (1.0/(float) Q) : (1.0/(float) P);
		*lpf = createLPF(cutoff_freq, taps, P);
	}

	if (!*buf) {
		*buf = new signalVector();
	}

	if (!*hist);
		*hist = new signalVector(hist_len);
}

/* Resample a signal vector
 *
 * The input vector is deallocated and the pointer returned with a vector
 * of any unconverted samples.
 */
signalVector *resmpl_sigvec(signalVector *hist, signalVector **vec,
			    signalVector *lpf, double in_rate,
			    double out_rate, int chunk_sz)
{
	signalVector *resamp_vec;
	int num_chunks = (*vec)->size() / chunk_sz;

	/* Truncate to a chunk multiple */
	signalVector trunc_vec(num_chunks * chunk_sz);
	(*vec)->segmentCopyTo(trunc_vec, 0, num_chunks * chunk_sz);

	/* Update sample buffer with remainder */
	*vec = segment(*vec, trunc_vec.size(), (*vec)->size() - trunc_vec.size());

	/* Add history and resample */
	signalVector input_vec(*hist, trunc_vec);
	resamp_vec = polyphaseResampleVector(input_vec, in_rate,
					     out_rate, lpf);

	/* Update history */
	trunc_vec.segmentCopyTo(*hist, trunc_vec.size() - hist->size(),
				hist->size());
	return resamp_vec;
}

/* Wrapper for receive-side integer-to-float array resampling */
 int rx_resmpl_int_flt(float *smpls_out, short *smpls_in, int num_smpls)
{
	int num_resmpld, num_chunks;
	signalVector *convert_vec, *resamp_vec, *trunc_vec;

	if (!rx_lpf || !rx_vec || !rx_hist)
		init_resampler(&rx_lpf, &rx_vec, &rx_hist, false);

	/* Convert and add samples to the receive buffer */
	convert_vec = short_to_sigvec(smpls_in, num_smpls);
	rx_vec = concat(rx_vec, convert_vec);

	num_chunks = rx_vec->size() / OUTCHUNK;
	if (num_chunks < 1)
		return 0;

	/* Resample */ 
	resamp_vec = resmpl_sigvec(rx_hist, &rx_vec, rx_lpf,
				   INRATE, OUTRATE, OUTCHUNK);
	/* Truncate */
	trunc_vec = segment(resamp_vec, INHISTORY,
                            resamp_vec->size() - INHISTORY);
	/* Convert */
	num_resmpld = sigvec_to_float(trunc_vec, smpls_out);

	return num_resmpld; 
}

/* Wrapper for transmit-side float-to-int array resampling */
int tx_resmpl_flt_int(short *smpls_out, float *smpls_in, int num_smpls)
{
	int num_resmpl, num_chunks;
	signalVector *convert_vec, *resamp_vec;

	if (!tx_lpf || !tx_vec || !tx_hist)
		init_resampler(&tx_lpf, &tx_vec, &tx_hist, true);

	/* Convert and add samples to the transmit buffer */
	convert_vec = float_to_sigvec(smpls_in, num_smpls);
	tx_vec = concat(tx_vec, convert_vec);

	num_chunks = tx_vec->size() / INCHUNK;
	if (num_chunks < 1)
		return 0;

	/* Resample and convert to an integer array */
	resamp_vec = resmpl_sigvec(tx_hist, &tx_vec, tx_lpf,
				   OUTRATE, INRATE, INCHUNK);
	num_resmpl = sigvec_to_short(resamp_vec, smpls_out);

	return num_resmpl; 
}

/* Receive a timestamped chunk from the device */ 
void RadioInterface::pullBuffer()
{
	int num_cv, num_rd;
	bool local_underrun;

	/* Read samples. Fail if we don't get what we want. */
	num_rd = mRadio->readSamples(rx_buf, OUTCHUNK, &overrun,
				     readTimestamp, &local_underrun);

	LOG(DEBUG) << "Rx read " << num_rd << " samples from device";
	assert(num_rd == OUTCHUNK);

	underrun |= local_underrun;
	readTimestamp += (TIMESTAMP) num_rd;

	/* Convert and resample */
	num_cv = rx_resmpl_int_flt(rcvBuffer + 2 * rcvCursor,
				   rx_buf, num_rd);

	LOG(DEBUG) << "Rx read " << num_cv << " samples from resampler";

	rcvCursor += num_cv;
}

/* Send a timestamped chunk to the device */ 
void RadioInterface::pushBuffer()
{
	int num_cv, num_wr;

	if (sendCursor < INCHUNK)
		return;

	LOG(DEBUG) << "Tx wrote " << sendCursor << " samples to resampler";

	/* Resample and convert */
	num_cv = tx_resmpl_flt_int(tx_buf, sendBuffer, sendCursor);
	assert(num_cv > sendCursor);

	if (pa.state()){
	  /* Write samples. Fail if we don't get what we want. */
	  num_wr = mRadio->writeSamples(tx_buf + OUTHISTORY * 2,
					num_cv - OUTHISTORY,
					&underrun,
					writeTimestamp);
	  
	  LOG(DEBUG) << "Tx wrote " << num_wr << " samples to device";
	  //this is weird -k
	  assert(num_wr == num_wr);
	}

	writeTimestamp += (TIMESTAMP) num_wr;
	sendCursor = 0;
}
