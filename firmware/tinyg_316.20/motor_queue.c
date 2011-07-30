/*
 * motor_queue.c - routines for managing motor moves
 * Part of TinyG project
 *
 * Copyright (c) 2010 - 2011 Alden S. Hart Jr.
 *
 * TinyG is free software: you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, 
 * or (at your option) any later version.
 *
 * TinyG is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. 
 * See the GNU General Public License for details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with TinyG  If not, see <http://www.gnu.org/licenses/>.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* ------
 * This module buffers pre-computed moves including line segments, 
 * dwells, stop and start commands. It manages the buffers that 
 * are consumed by the stepper routines.
 */

#include <stdlib.h>
#include <string.h>					// for memset();
#include <math.h>
#include <stdio.h>					// precursor for xio.h
#include <avr/pgmspace.h>			// precursor for xio.h
#include <avr/interrupt.h>

#include "system.h"
#include "xio.h"					// supports TRAP and print statements
#include "tinyg.h"
#include "util.h"

#include "motor_queue.h"
#include "planner.h"
#include "stepper.h"				// for 	st_execute_move()
#include "config.h"
#include "help.h"

#define MQ_BUFFER_SIZE 3 // limited to 255 unless heads & tails --> uint16_t

struct mqSingleton {
	volatile uint8_t head;			// motor queue index (for writes)
	volatile uint8_t tail;			// motor dequeue index (for reads)
	uint32_t previous_ticks;		// tick count from previous move
	struct mqMove *p;				// motor buffer pointer
	struct mqMove move_buffer[MQ_BUFFER_SIZE];// motor buffer storage
};
static struct mqSingleton mq;

// local functions
static uint8_t _mq_request_load(uint8_t type);
static void _mq_set_f_dda(double *f_dda, double *dda_substeps, 
						  const double major_axis_steps, 
						  const double microseconds);

/* 
 * mq_init() - initialize move buffers
 */

void mq_init()
{
	mq.head = 0;
	mq.tail = MQ_BUFFER_SIZE-1;
}

/**** MOTOR QUEUE ROUTINES ****
 * mq_test_motor_buffer() 	 - test if motor buffer is available for write
 * mq_queue_motor_buffer() 	 - get and queue write buffer
 * mq_dequeue_motor_buffer() - dequeue read buffer
 * mq_flush_motor_buffer() 	 - remove all buffered moves (reset queue) 
 * 
 * Move buffer circular buffer operation
 *	mq.head is the array index to which the move will be queued (written)
 *	mq.head is post-incremented (after queuing the move)
 *  mq.tail is the array index from which the previous move was dequeued
 *	mq.tail is pre-incremented (before dequeuing the move)
 *
 *	Buffer empty:	move_buffer_head == move_buffer_tail
 *	Buffer full:	move_buffer_head+1 == move_buffer_tail 
 *
 * Note: The method of testing for buffer full then writing the buffer 
 *		 as a separate, non-atomic operation works as long as there is 
 *		 no pre-emption that could invalidate the full/not-full result. 
 *		 As this project is currently coded there is no pre-emption 
 *		 possible in this critical region - i.e. there's no way somebody 
 *		 else can get in there and write to the move buffer between the 
 *		 not-full result and the subsequent write. 
 *
 * Note: Discovered a race condition where a buffer can be "queued" by 
 *		 mq_queue_line() and before it's been filled it's loaded by 
 *		 _st_load_move(). This causes values to be wacko, and potentially
 *		 the timer period to be set to zero. Not good. mq_state was 
 *		 introduced to provide a mutex to prevent this from occurring.
 */

uint8_t mq_test_motor_buffer() 
{
	if (mq.head == mq.tail) { 
		return FALSE; 				// buffer full
	}
	return (TRUE);
}

struct mqMove * mq_queue_motor_buffer() 
{
	if (mq.tail == mq.head) { 
		return (NULL);				// buffer full
	} 
	mq.p = &mq.move_buffer[mq.head];
	if (++mq.head >= MQ_BUFFER_SIZE) { 
		mq.head = 0; 				// advance head
	}
	mq.p->mq_state = MQ_BUFFER_LOADING;
	return (mq.p);
}

struct mqMove * mq_dequeue_motor_buffer()
{
	uint8_t next_tail = mq.tail;
	if (++next_tail >= MQ_BUFFER_SIZE) { 
		next_tail = 0; 				// incr with wrap
	}
	if (next_tail == mq.head) { 
		return (NULL); 				// buffer empty
	}
	mq.tail = next_tail;
	return (mq.p = &mq.move_buffer[mq.tail]);
}

void mq_flush_motor_buffer()
{
	cli();
	mq.tail = mq.head;
	mq.p = NULL;
	sei();
}

/*
 * mq_queue_line() - Add a new linear movement to the move buffer
 *
 * This function queues a line segment to the motor buffer. It works 
 * in joint space (motors) and it works in steps, not length units.
 * 
 * It deals with all the DDA optimizations and timer setups *here* so 
 * that the dequeuing operation can be as rapid as possible. All args 
 * are provided as doubles and converted to their appropriate integer 
 * types during queuing.
 *
 * Args:
 *	steps_x ... steps_a are signed relative motion in steps
 *	Microseconds specifies how many microseconds the move should take 
 *	 (Note that these are constant speed segments being queued)
 *
 * Blocking behavior:
 *	This routine returns BUFFER_FULL if there is no space in the buffer.
 *  A blocking version should wrap this code with blocking semantics
 */

uint8_t mq_queue_line(double steps[], double microseconds)
{
	uint8_t i;
	double length = 0;
	double f_dda = F_DDA;	// starting point for adjustment
	double dda_substeps = DDA_SUBSTEPS;
	double major_axis_steps = 0;

#ifdef __dbSHOW_QUEUED_LINE
	fprintf_P(stderr, PSTR("Queue line %6.1f %6.1f %6.1f %6.1f - %6.0f\n"), 
			  steps_x, steps_y, steps_z, steps_a, microseconds);
#endif

	// trap zero time and infinite time moves
	if ((isfinite(microseconds) == FALSE) || (microseconds < EPSILON)) {
		return (TG_ZERO_LENGTH_MOVE);
	}
	// trap zero step moves - and get the major axis while you're at it
 	// Also, test the zero-length case: vector > 1 but all axes are < 1
	for (i=0; i<MOTORS; i++) {
		length += square(steps[i]);
		if (major_axis_steps < fabs(steps[i])) { 
			major_axis_steps = fabs(steps[i]); 
		}
	}
	if ((sqrt(length) < 1) || (major_axis_steps < 1)) {
		return (TG_ZERO_LENGTH_MOVE);
	}
	// get the buffer last - so you don't have to free in in case of error
	if ((mq.p = mq_queue_motor_buffer()) == NULL) {	// shouldn't ever fail
		return (TG_BUFFER_FULL_NON_FATAL);			//...but just in case
	}
	// set dda clock frequency and substeps
	_mq_set_f_dda(&f_dda, &dda_substeps, major_axis_steps, microseconds);

	// setup motor parameters
	for (i=0; i<MOTORS; i++) {
		mq.p->a[i].dir = ((steps[i] < 0) ? 1 : 0) ^ cfg.m[i].polarity;
		mq.p->a[i].steps = (uint32_t)fabs(steps[i] * dda_substeps);
	}
	mq.p->timer_period = _f_to_period(f_dda);
	mq.p->timer_ticks = (uint32_t)((microseconds/1000000) * f_dda);
	mq.p->timer_ticks_X_substeps = (uint32_t)((microseconds/1000000) * f_dda * dda_substeps);

	if ((mq.p->timer_ticks * COUNTER_RESET_FACTOR) < mq.previous_ticks) {  // uint32_t math
		mq.p->counter_reset_flag = TRUE;
	} else {
		mq.p->counter_reset_flag = FALSE;
	}
	mq.previous_ticks = mq.p->timer_ticks;
	return(_mq_request_load(MQ_LINE)); 	// label it and load it (or at least try to)
}

/* 
 * _mq_set_f_dda() - get optimal DDA frequency setting
 *
 *	Find the highest integer multiple of the major axis step rate that is
 *	less than DDA max frequency and no more than OVERCLOCK times the
 *	step rate; or use the min DDA frequency if the step rate is too low.
 *	Test that the selected rate will fit into a long (i.e. won't overflow 
 *	uint32_t timer_ticks_scaled). If it doesn't fit reduce the substep 
 *	precision until it does. If it *still* doesn't fit get rid of the 
 *	overclocking. If it **still** doesn't fit throw a trap and give up.
 */
static void _mq_set_f_dda(double *f_dda,
						  double *dda_substeps, 
						  const double major_axis_steps, 
						  const double microseconds)
{
	double f_dda_base = (major_axis_steps / microseconds) * 1000000;

	// chose a good clack value, assuming the line will fit
	if (DDA_OVERCLOCK == 0) { 						// disabled
		*f_dda = f_dda_base;
	} else if ((f_dda_base * DDA_OVERCLOCK) < F_DDA_MIN) {	// too slow
		*f_dda = F_DDA_MIN;
	} else {
		for (uint8_t dda_overclock=DDA_OVERCLOCK; dda_overclock>0; dda_overclock--) {
			if ((*f_dda = (f_dda_base * dda_overclock)) <  F_DDA) {
				break;
			}
		}
	}
	// reduce substep precision if line won't fit into timer_ticks_scaled equiv to
	// this expr: ((microseconds/1000000) *(*f_dda) *(*dda_substeps)) > MAX_ULONG) {
	while ((microseconds *(*f_dda) *(*dda_substeps)) > (MAX_ULONG * 1000000)) {
		if (((*dda_substeps) = (*dda_substeps)/2) < 1) {
			(*dda_substeps) = 1;
			// dang. still need more room. kill the overclock
			if (((*f_dda) = f_dda_base) < F_DDA_MIN) {
				 *f_dda = F_DDA_MIN;
			}
			if ((microseconds *(*f_dda) *(*dda_substeps)) > (MAX_ULONG * 1000000)) {
				TRAP1(PSTR("_mq_set_f_dda() line overflow: %f"), major_axis_steps)
//+++++ DEBUG	dump_set_f_dda(*f_dda, *dda_substeps, major_axis_steps, microseconds, f_dda_base);
				break;
			}
		}
	}
//	dump_set_f_dda(*f_dda, *dda_substeps, major_axis_steps, microseconds, f_dda_base);
}

/*
 * mq_queue_dwell() - Add a dwell to the move buffer
 */

uint8_t mq_queue_dwell(double microseconds)
{
	if ((mq.p = mq_queue_motor_buffer()) == NULL) {	// should always get a buffer
		return (TG_BUFFER_FULL_NON_FATAL);
	}
	mq.p->timer_period = _f_to_period(F_DWELL);
	mq.p->timer_ticks = (uint32_t)((microseconds/1000000) * F_DWELL);
	return(_mq_request_load(MQ_DWELL));
}

/*
 * mq_queue_stops() - Add a start, stop or end to the move buffer
 */

uint8_t mq_queue_stops(uint8_t mq_type)
{
	if ((mq.p = mq_queue_motor_buffer()) == NULL) {	// should always get a buffer
		return (TG_BUFFER_FULL_NON_FATAL);
	}
	return(_mq_request_load(mq_type));
}

/*
 * _mq_request_load() - put buffer on queue and request stepper load
 */

static uint8_t _mq_request_load(uint8_t type)
{
	mq.p->mq_type = type;	
	mq.p->mq_state = MQ_BUFFER_READY;
	st_request_load();
	return(TG_OK);
}

/*
 * mq_print_motor_queue()
 */
/*
static char mqs_mbuf[] PROGMEM = "MotorBuffer head %d,  tail %d\n";
static char mqs_move[] PROGMEM = "  [%d]: move_type:%d  timer_period:%d  timer_ticks:%d\n";
static char mqs_motr[] PROGMEM = "     [%d]: dir:%d  steps:%d\n";

void mq_print_motor_queue()
{
	uint8_t i,j;

	fprintf_P(stderr, (PGM_P)mqs_mbuf, mq.head, mq.tail);
	for (i=0; i<MQ_BUFFER_SIZE; i++) {
		fprintf_P(stderr, (PGM_P)mqs_move, i,
			mq.move_buffer[i].mq_type, 
			mq.move_buffer[i].timer_period, 
			mq.move_buffer[i].timer_ticks);
		for (j=0; j<MOTORS; j++) {
			fprintf_P(stderr, (PGM_P)mqs_motr, j, 
				mq.move_buffer[i].a[j].dir,
				mq.move_buffer[i].a[j].steps);
		}
	}
	st_print_stepper_state();
}
*/

//############## UNIT TESTS ################

#ifdef __UNIT_TESTS

void _mp_test_medium_line(void);
void _mp_test_short_line(void);
void _mp_test_very_short_line(void);
void _mp_test_short_med_short(void);

void mq_unit_tests()
{
//	_mp_test_medium_line();
//	_mp_test_short_line();
//	_mp_test_very_short_line();
	_mp_test_short_med_short();
}

void _mp_test_medium_line() 
{
	mq_queue_line(100, 111, 123, 0, 1000000);// x, y, z, a, microseconds
}

void _mp_test_short_line() 
{
	mq_queue_line(10, 10, 10, 0, 10000);	// x, y, z, a, microseconds
}

void _mp_test_very_short_line() 
{
	mq_queue_line(0.1, 0.2, 0.3, 0, 10000);
}

void _mp_test_short_med_short() 
{
	_mp_test_short_line();
	_mp_test_medium_line();
	_mp_test_short_line();
}

#endif
