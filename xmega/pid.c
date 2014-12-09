#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pid.h"
#include "pololu.h"

#define constP            120
#define constI            0
#define constD            0.0004
#define PID_HISTORY_SIZE  8
#define PID_HISTORY_LOG   3

// Macros to help convert rads/s to ticks and vice versa
#define ticks_to_rads(x) ((x) * (radPerTick/sampleTime)) 
#define rads_to_ticks(x) ((x) * (sampleTime/radPerTick))

#define TEST_CPU 32000000

// Configuration Values
static TC0_t *pid_tick_timer = &TCC0;
#define PID_TICK_OVF TCC0_OVF_vect

// PORTs to be used with wheel encoders
//	Once port can handle interrupts for 2 encoders
static PORT_t *wheelPort1 = &PORTE;
static PORT_t *wheelPort2 = &PORTB;

static float radPerTick = (2*M_PI)/1856.0;
static const float sampleTime = 10e-3;

static pololu_t pololu_LF = {
	.PORT = &PORTD,
	.TC2 = &TCD2,
	.motor2 = 0,
};
static pololu_t pololu_RF = {
	.PORT = &PORTD,
	.TC2 = &TCD2,
	.motor2 = 1,
};
static pololu_t pololu_RR = {
	.PORT = &PORTF,
	.TC2 = &TCF2,
	.motor2 = 0,
};
static pololu_t pololu_LR = {
	.PORT = &PORTF,
	.TC2 = &TCF2,
	.motor2 = 1,
};

//initialize all of the history queues
static Error_History leftfront_history = {
  .data = 0,
  .start = ERROR_QUEUE_SIZE-1,
}; 
static Error_History leftrear_history = {
  .data = 0,
  .start = ERROR_QUEUE_SIZE-1,
}; 
static Error_History rightfront_history = {
  .data = 0,
  .start = ERROR_QUEUE_SIZE-1,
}; 
static Error_History rightrear_history = {
  .data = 0,
  .start = ERROR_QUEUE_SIZE-1,
}; 

// File-Scope Variables and Structures
//	AVG_speed: Average speed of given wheel in rads/S
//	ticks: Measurement of current ticks for given sample
//	output: Current output of PID controller per wheel
//	setpoint: The desired speed for the PID controller
//	errSum: running sum of speed error per wheel
//	pid_last_ticks: Measurement of ticks from previous sample

typedef struct{
	/*	Values for measuring the speed of the wheel
	 *	AVG_speed:		Current running speed measured in rads/s
	 *	ticks:			Current number of ticks per given sample
	 *	output:			Current output of PID controller
	 *	setpoint:		Desired speed (in rads/s) for given wheel
	 *	kp:				PID proportional gain
	 *	ki:				PID integral gain
	 *	kd:				PID derivative gain
	 *	pid_last_ticks:	Number of ticks from the previous sample
	 *  odometry_last_ticks: No idea
	 */
	
	volatile int16_t AVG_speed;
	int16_t setpoint;
	volatile uint16_t ticks;
	uint16_t pid_last_ticks;
	int16_t output;
	// Values for the actual pid controller
	float kp, ki, kd;
	
	uint16_t odometry_last_ticks; // we ain't there yet
} pid_wheel_data_t;

static const pid_wheel_data_t DEFAULT = {
	.AVG_speed = 0,
	.output = 0,
	.setpoint = 0,
	.kp = 0,
	.ki = 0,
	.kd = 0,
	.pid_last_ticks =0,
	.odometry_last_ticks = 0
};

// Array of wheel data (per wheel?)
static pid_wheel_data_t wheelData[4];

void pid_init() {  
	//wheelPort1 = PORTA;
	//wheelPort2 = PORTB;
  
  //setup the queue buffers if not declared
  if(!leftfront_history.data) leftfront_history.data = malloc(2*ERROR_QUEUE_SIZE);
  if(!rightfront_history.data) rightfront_history.data = malloc(2*ERROR_QUEUE_SIZE);
  if(!leftrear_history.data) leftrear_history.data = malloc(2*ERROR_QUEUE_SIZE);
  if(!rightrear_history.data) rightrear_history.data = malloc(2*ERROR_QUEUE_SIZE);
    
	//Initialize Pololus
	pololuInit(&pololu_LF);
	pololuInit(&pololu_RF);
	pololuInit(&pololu_RR);
	pololuInit(&pololu_LR);

	//Initialize wheelData to default values
	for(int8_t x = 0; x < 4; x++)
		wheelData[x] = DEFAULT;

	// Initialize motor frequency measurement timers.
	wheelPort1->DIRCLR = 0x0F;
	wheelPort1->PIN0CTRL = PORT_ISC_BOTHEDGES_gc;
	wheelPort1->INT0MASK = 0x05;
	wheelPort1->INTCTRL = PORT_INT0LVL_LO_gc | PORT_INT1LVL_LO_gc;
	wheelPort1->PIN1CTRL = PORT_ISC_BOTHEDGES_gc;
	wheelPort1->INT1MASK = 0x0A;

	wheelPort2->DIRCLR = 0x0F;
	wheelPort2->PIN0CTRL = PORT_ISC_BOTHEDGES_gc;
	wheelPort2->INT0MASK = 0x05;
	wheelPort2->INTCTRL = PORT_INT0LVL_LO_gc | PORT_INT1LVL_LO_gc;
	wheelPort2->PIN1CTRL = PORT_ISC_BOTHEDGES_gc;
	wheelPort2->INT1MASK = 0x0A;

	// Initialize the PID tick timer
	// Uses timer on portC
	pid_tick_timer->CTRLA = TC_CLKSEL_DIV64_gc;
	pid_tick_timer->CTRLB = 0x00;
	pid_tick_timer->CTRLC = 0x00;
	pid_tick_timer->CTRLD = 0x00;
	pid_tick_timer->CTRLE = 0x00;
	pid_tick_timer->PER = round(TEST_CPU * sampleTime / 64.);
	pid_tick_timer->INTCTRLA = TC_OVFINTLVL_LO_gc;

	// Set PID gain defaults per wheel
	pid_setTunings(constP, constI, constD, LEFT_FRONT_MOTOR);
	pid_setTunings(constP, constI, constD, LEFT_REAR_MOTOR);
	pid_setTunings(constP, constI, constD, RIGHT_FRONT_MOTOR);
	pid_setTunings(constP, constI, constD, RIGHT_REAR_MOTOR);
}

uint16_t pid_compute(uint8_t motor) {
	// Compute all working error variables
	int16_t errors[PID_HISTORY_SIZE];
  error_history_batch(errors, PID_HISTORY_SIZE, motor);
	
  //derivative term is difference / size of history * kd
  float dTerm = ((errors[PID_HISTORY_SIZE-1] - errors[0]) \
    >> PID_HISTORY_LOG) * wheelData[motor].kd;
    
  int16_t iTermSum = 0; //integral term is sum of error history
  for(int i=0;i<PID_HISTORY_SIZE;++i) iTermSum += errors[i];
  float iTerm = iTermSum * wheelData[motor].ki; //multiply by punishment
  
	// Compute the output (sum of p,i,and d)
	return (wheelData[motor].kp * errors[PID_HISTORY_SIZE - 1]) + iTerm + dTerm;
}

// Set the default PID gain values for a given wheel
void pid_setTunings(float Kp, float Ki, float Kd, uint8_t motor) {
	wheelData[motor].kp = Kp;
	wheelData[motor].ki = Ki * sampleTime;
	wheelData[motor].kd = Kd / sampleTime;
}

static void pid_measureSpeed(uint8_t motor) {
	// Pull current number of ticks for given motor 'num'
	uint16_t ticks = wheelData[motor].ticks;
	// Average Speed would be the change in ticks
	wheelData[motor].AVG_speed = ticks - wheelData[motor].pid_last_ticks;
	// Replace the current number ticks as the previous number of ticks
	wheelData[motor].pid_last_ticks = ticks;
}

float pid_getSpeed(uint8_t motor) {
	return wheelData[motor].AVG_speed;
}

void pid_setSpeed(int16_t speed, uint8_t motor) {
	wheelData[motor].setpoint = speed;
}

/*******************************************************************
-----> Handler Functions <-----
*******************************************************************/
// Handler that sets wheel speeds based on message.
// Arguments:
// message := [Wheel1B0, Wheel1B1, Wheel1B2, Wheel1B3, ... , Wheel4B0, Wheel4B1, Wheel4B2, Wheel4B3]
// 16 bytes, little endian 32bit numbers that represent the desired wheel speed
// multiplied by 1000. Pass a pointer to the low byte of
// len := the length of the message. E.g. the number of bytes in the array
int pid_speed_msg(Message msg) {
	/*
		Will be used as callback to set current desired wheel speeds
	*/
	float* data = (float*) msg.data;
	wheelData.setpoint[LEFT_FRONT_MOTOR] = (uint16_t) rads_to_ticks(data[0]);
	wheelData.setpoint[RIGHT_FRONT_MOTOR] = (uint16_t) rads_to_ticks(data[1]);
	wheelData.setpoint[RIGHT_REAR_MOTOR] = (uint16_t) rads_to_ticks(data[2]);
	wheelData.setpoint[LEFT_REAR_MOTOR] = (uint16_t) rads_to_ticks(data[3]);
	}

static void pid_get_odometry(float* returnData) {
	for(int i = 0; i < 4; i++) {
		int16_t ticks = wheelData[i].ticks;
		volatile int16_t tmp = ticks - wheelData[i].odometry_last_ticks;
		volatile float tmp2 = tmp * radPerTick;
		returnData[i] = tmp2;
		wheelData[i].odometry_last_ticks = ticks;
	}
}

static inline void pid_set_speed_multiplier(float val) {
	radPerTick = val;
}

static inline float pid_get_speed_multiplier(void) {
	return radPerTick;
}

// Because the multiplier is a floating point value, we'll multiply it by 1000 first, and then send it.
// Or, if we're receiving it, we'll divide it by 1000.
/*
void pid_get_speed_multiplier_handler(char* message, uint8_t len) {
	char multiplier[2];
	uart_float_to_char16(multiplier, pid_get_speed_multiplier());
	uart_send_msg_block(PIDgetMultiplier, multiplier, 3);
}
*/

/*
void pid_set_speed_multiplier_handler(char* message, uint8_t len) {
	pid_set_speed_multiplier(uart_int16_to_float(message));
}
*/

ISR(PID_TICK_OVF) {
	// Push errors to history
	error_history_push(pid_measureSpeed(LEFT_FRONT_MOTOR), LEFT_FRONT_MOTOR);
	error_history_push(pid_measureSpeed(RIGHT_FRONT_MOTOR), RIGHT_FRONT_MOTOR);
	error_history_push(pid_measureSpeed(RIGHT_REAR_MOTOR), RIGHT_REAR_MOTOR);
	error_history_push(pid_measureSpeed(LEFT_REAR_MOTOR), LEFT_REAR_MOTOR);
}

unsigned int grayToBinary(unsigned int num)
{
	unsigned int mask;
	for (mask = num >> 1; mask != 0; mask = mask >> 1)
	{
		num = num ^ mask;
	}
	return num;
}

//push an error sample to specified queue
void error_history_push(int16_t data, uint8_t motor){ 
  Error_History* error;
  switch (motor){
    case LEFT_FRONT_MOTOR:  error = &leftfront_history;
    case LEFT_REAR_MOTOR:   error = &leftrear_history;
    case RIGHT_FRONT_MOTOR: error = &rightfront_history;
    case RIGHT_REAR_MOTOR:  error = &rightrear_history;
  }
  error->data[error->start] = data;
  error->start = (error->start + 1) % ERROR_QUEUE_SIZE;
}

//get a single history entry
int16_t error_history_at(int8_t index, uint8_t motor){ 
  Error_History* error;
  switch (motor){
    case LEFT_FRONT_MOTOR:  error = &leftfront_history;
    case LEFT_REAR_MOTOR:   error = &leftrear_history;
    case RIGHT_FRONT_MOTOR: error = &rightfront_history;
    case RIGHT_REAR_MOTOR:  error = &rightrear_history;
  }
  return error->data[error->start];
}

//return batched history entries
void error_history_batch(int16_t* buffer, uint8_t size, uint8_t motor){ 
  Error_History* error;
  switch (motor){
    case LEFT_FRONT_MOTOR:  error = &leftfront_history;
    case LEFT_REAR_MOTOR:   error = &leftrear_history;
    case RIGHT_FRONT_MOTOR: error = &rightfront_history;
    case RIGHT_REAR_MOTOR:  error = &rightrear_history;
  }
  
  if(error->start - size < 0){ //check value range
    int overflow_addr = 64 + error->start - size;
    int overflow_amt = size-error->start-1;
    memcpy(buffer, error->data+overflow_addr, overflow_amt);
    memcpy(buffer+overflow_amt, error->data, error->start+1); //copy newer data
  } else { //no overflow
    memcpy(buffer, error->data + error->start, size);
  }
}

//call a pid update
void update_pid(void){ 
  //do the maths
	pololu_set_velocity(&pololu_LF, pid_compute(LEFT_FRONT_MOTOR));
	pololu_set_velocity(&pololu_RF, pid_compute(RIGHT_FRONT_MOTOR));
	pololu_set_velocity(&pololu_RR, pid_compute(RIGHT_REAR_MOTOR));
	pololu_set_velocity(&pololu_LR, pid_compute(LEFT_REAR_MOTOR));

}

ISR(PORTE_INT0_vect){
	// pins 0 and 2
	static int8_t old_value = 0;
	int8_t new_value = grayToBinary(((wheelPort1->IN & 4) >> 1) | (wheelPort1->IN & 1));
	int8_t difference = new_value - old_value;
	if(difference > 2) difference -= 4;
	if(difference < -2) difference += 4;
	wheelData[LEFT_FRONT_MOTOR].ticks += difference;
	old_value = new_value;
}

ISR(PORTE_INT1_vect){
	// pins 1 and 3
	static int8_t old_value = 0;
	int8_t new_value = grayToBinary(((wheelPort1->IN & 8) >> 2) | ((wheelPort1->IN & 2) >> 1));
	int8_t difference = new_value - old_value;
	if(difference > 2) difference -= 4;
	if(difference < -2) difference += 4;
	wheelData[RIGHT_FRONT_MOTOR].ticks += difference;
	old_value = new_value;
}

ISR(PORTB_INT0_vect){
	// pins 0 and 2
	static int8_t old_value = 0;
	int8_t new_value = grayToBinary(((wheelPort2->IN & 4) >> 1) | (wheelPort2->IN & 1));
	int8_t difference = new_value - old_value;
	if(difference > 2) difference -= 4;
	if(difference < -2) difference += 4;
	wheelData[RIGHT_REAR_MOTOR].ticks += difference;
	old_value = new_value;
}

ISR(PORTB_INT1_vect){
	// pins 1 and 3
	static int8_t old_value = 0;
	int8_t new_value = grayToBinary(((wheelPort2->IN & 8) >> 2) | ((wheelPort2->IN & 2) >> 1));
	int8_t difference = new_value - old_value;
	if(difference > 2) difference -= 4;
	if(difference < -2) difference += 4;
	wheelData[LEFT_REAR_MOTOR].ticks += difference;
	old_value = new_value;
}