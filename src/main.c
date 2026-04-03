/* Authors: Bashar Jirjees & Nicholas McLennan*/

/* Importing the Needed Modules */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32f4xx.h"
#include "stm32f4_discovery.h"
#include "stm32f4xx_adc.h"
#include "stm32f4xx_gpio.h"

#include "../FreeRTOS_Source/include/FreeRTOS.h"
#include "../FreeRTOS_Source/include/queue.h"
#include "../FreeRTOS_Source/include/semphr.h"
#include "../FreeRTOS_Source/include/task.h"
#include "../FreeRTOS_Source/include/timers.h"

/* Tasks Test Batch Number */
int chosen_test_batch = 3;

/* Defining Priorities and Delay Values*/
#define MAX_PRIORITY   		4
#define HIGH_PRIORITY       3
#define LOW_PRIORITY        2
#define MIN_PRIORITY        1
#define QUEUE_DELAY 0XFFFFFFFF
#define SEMAPHORE_DELAY 0XFFFFFFFF

/* Defining the Fixed Task Types */
typedef enum {PERIODIC, APERIODIC} task_type;

/* Defining the Fixed Message Types */
typedef enum {RELEASE, COMPLETE, ACTIVE_TASKS, COMPLETED_TASKS, OVERDUE_TASKS} message_type;

/* Defining the Task Contents */
struct dd_task{
    TaskHandle_t t_handle;
    task_type type;
    uint32_t task_id;
    uint32_t release_time;
    uint32_t absolute_deadline;
    uint32_t completion_time;
};

/* Defining the Task List Contents */
struct dd_task_list{
    struct dd_task task;
    struct dd_task_list *next_task;
};

/* Defining the Message Contents */
typedef struct {
    message_type m_type;
    TaskHandle_t t_handle;
    task_type type;
    uint8_t task_id;
    uint32_t completion_time;
    uint32_t task_counter_id;
    uint32_t absolute_deadline;
    struct dd_task_list* task_list;
} task_message;

/* Defining the Task Type Lists */
struct dd_task_list* active_list_head = NULL;
struct dd_task_list* completed_list_head = NULL;
struct dd_task_list* overdue_list_head = NULL;

/* Defining the Queues that Stores All Tasks */
QueueHandle_t tasks_queue;
QueueHandle_t prev_exec;
QueueHandle_t lists_queue;

/* Defining the Generator and Monitor Tasks Variables */
TaskHandle_t generator_handle;
TaskHandle_t monitor_handle;

/* List Used to Index the Status of the Completed Task Based on Completion Time and Deadline */
char *completed_overdue[2] = {"Completed", "Overdue"};

/*
    This Function Takes the Newly Generated Task and Adds it to the All Tasks Queue
    The New Task Queue Location Depends on Its Deadline, The Shorter the Deadline,
    the Closer the New Task to Position 0 in the Queue (Less Time to Pop from the Queue)

    Input: Active Tasks List and the Newly Added Task
    Return: None
*/

void insert_task(struct dd_task_list** head, struct dd_task_list* new_task) {

    struct dd_task_list* current;
    if (*head == NULL || (*head)->task.absolute_deadline > new_task->task.absolute_deadline) {
        new_task->next_task = *head;
        *head = new_task;
    } else {
        current = *head;
        while (current->next_task != NULL && current->next_task->task.absolute_deadline <= new_task->task.absolute_deadline) {
            current = current->next_task;
        }
        new_task->next_task = current->next_task;
        current->next_task = new_task;
    }
}

/*
    This Function Removes a Specific Completed Task Based on Its ID From the Active Tasks List
    The Tasks Doesn't Perform any Sorting

    Input: Active Tasks List and the Completed Task ID
    Return: None
*/

struct dd_task_list* remove_task(struct dd_task_list** head, uint32_t id) {

    struct dd_task_list* temp = *head;
    struct dd_task_list* prev = NULL;

    if (temp != NULL && temp->task.task_id == id) {
        *head = temp->next_task;
        temp->next_task = NULL;
        return temp;
    }
    while (temp != NULL && temp->task.task_id != id) {
        prev = temp;
        temp = temp->next_task;
    }
    if (temp == NULL) return NULL;
    prev->next_task = temp->next_task;
    temp->next_task = NULL;

    return temp;
}

/*
    This Function Sets the Task with the Lowest Deadline as the Highest Priority and
    Other Tasks as Low Priority; Low Priority > Min Priority

    Input: None
    Return: None
*/

void update_tasks_priorities(void) {

    struct dd_task_list* current = active_list_head;
    if (current != NULL) {

        vTaskPrioritySet(current->task.t_handle, HIGH_PRIORITY);
        if(eTaskGetState(current->task.t_handle) == eSuspended || eTaskGetState(current->task.t_handle) == eBlocked)
        	vTaskResume(current->task.t_handle);

        current = current->next_task;
        while (current != NULL) {

        	vTaskPrioritySet(current->task.t_handle, MIN_PRIORITY);
        	vTaskSuspend(current->task.t_handle);

            current = current->next_task;
        }
    }
}

/*
    This Function Creates Structure Based on the Newly Generated Task Parameters and
    Adds it to the All Tasks Queue

    Input: Newly Generated Task, Task Type, Task ID, and Task Execution Deadline
    Return: None
*/

void create_dd_task(TaskHandle_t t_handle, task_type type, uint32_t task_counter_id, uint32_t task_id, uint32_t absolute_deadline) {

    task_message msg;
    msg.m_type = RELEASE;
    msg.t_handle = t_handle;
    msg.type = type;
    msg.task_id = task_id;
    msg.task_counter_id = task_counter_id,
    msg.absolute_deadline = absolute_deadline;

    xQueueSend(tasks_queue, &msg, pdMS_TO_TICKS(QUEUE_DELAY));
}

/*
    This Function Adds the Completed Tasks Again to The All Tasks Queue But with Complete State
    To be Removed

    Input: Task ID
    Return: None
*/

void delete_dd_task(uint32_t task_counter_id, uint8_t task_id, uint32_t completion_time, uint32_t deadline) {

    task_message msg;

    msg.m_type = COMPLETE;
    msg.task_id = task_id;
    msg.completion_time = completion_time;
    msg.absolute_deadline = deadline;
    msg.task_counter_id = task_counter_id;

    xQueueSend(tasks_queue, &msg, pdMS_TO_TICKS(QUEUE_DELAY));
}

/*
    This Function Returns the List of the Currently Active Tasks

    Input: None
    Return: The Active Tasks List Head
*/

struct dd_task_list* get_active_dd_task_list(void) {

    task_message msg;
    struct dd_task_list* active_list_head;
    msg.m_type = ACTIVE_TASKS;

    xQueueSend(tasks_queue, &msg, pdMS_TO_TICKS(0));
    xQueueReceive(lists_queue, &active_list_head, pdMS_TO_TICKS(100));

    return active_list_head;
}

/*
    This Function Returns the List of the Currently Completed Tasks

    Input: None
    Return: The Completed Tasks List Head
*/

struct dd_task_list* get_completed_dd_task_list(void) {

    task_message msg;
    struct dd_task_list* completed_list_head;
    msg.m_type = COMPLETED_TASKS;

    xQueueSend(tasks_queue, &msg, pdMS_TO_TICKS(0));
    xQueueReceive(lists_queue, &completed_list_head, pdMS_TO_TICKS(100));

    return completed_list_head;
}

/*
    This Function Returns the List of the Currently Overdue Tasks

    Input: None
    Return: The Overdue Tasks List Head
*/

struct dd_task_list* get_overdue_dd_task_list(void) {

    task_message msg;
    struct dd_task_list* overdue_list_head;
    msg.m_type = OVERDUE_TASKS;

    xQueueSend(tasks_queue, &msg, pdMS_TO_TICKS(0));
    xQueueReceive(lists_queue, &overdue_list_head, pdMS_TO_TICKS(100));

    return overdue_list_head;
}

/*
    This Function Display the Tasks Status, Release Time, Deadline, and Completion Time
    The function Also Summarizes the Total Number of Active, Completed and Overdue Tasks

    Input: Active/Completed/Overdue Tasks List and Task Fixed Status with That List
    Return: None
*/

void get_current_tasks(struct dd_task_list* head, char* task_status) {

		struct dd_task_list* current = head;

		uint32_t count = 0;

		char *total_count_status[] = {"GET_ACTIVE_TASK_COUNT", "GET_COMPLETED_TASK_COUNT", "GET_OVERDUE_TASK_COUNT"};

		while (current != NULL)
		{
            // Checking that the Semaphore is Available Before Increment Over Counting
			++count;
			current = current->next_task;

		}

       if (strcmp(task_status , total_count_status[0]) == 0) printf("A#: %i\n", (int)count);
       else if (strcmp(task_status , total_count_status[1]) == 0) printf("C#: %i\n", (int)count);
       else if (strcmp(task_status , total_count_status[2]) == 0) printf("O#: %i\n", (int)count);
}

/*
    This Function Checks the Tasks Status Regularly to Adjust their Position
    Within the Tasks List and Alters the Tasks Current Status based on
    Completion Time

    Input: None
    Return: None
*/

void vDDSTask(void *pvParameters) {

    task_message msg;
    uint32_t current_time;

    while (1) {

        //  Getting the Next Released Task Message in The Queue
        if (xQueueReceive(tasks_queue, &msg, pdMS_TO_TICKS(QUEUE_DELAY)) == pdTRUE){

                current_time = xTaskGetTickCount();

                // If a New Task is Released then it's Added to the Active Tasks List
                if(msg.m_type == RELEASE) {

                    struct dd_task_list* new_task = pvPortMalloc(sizeof(struct dd_task_list));

                    new_task->task.t_handle = msg.t_handle;
                    new_task->task.type = msg.type;
                    new_task->task.task_id = msg.task_counter_id;
                    new_task->task.release_time = current_time;
                    new_task->task.absolute_deadline = current_time + pdMS_TO_TICKS(msg.absolute_deadline);
                    new_task->task.completion_time = 0;
                    new_task->next_task = NULL;

                    insert_task(&active_list_head, new_task);

                    // Ensuring the Tasks Queue is Empty Before Updating New Tasks Priorities
                    if(xQueuePeek(tasks_queue, &msg, pdMS_TO_TICKS(0)) == pdFALSE)
                    	update_tasks_priorities();
                }

				// If a Task is Completed then it's Added to the Completed/Overdue Tasks List
				// The Completed Task is Removed from the Active Tasks List
				// The Completed Task is Also Checked for Completing After Deadline
			    // Newly Released Tasks Can Start Executing
                else if(msg.m_type == COMPLETE) {

                    struct dd_task_list* completed_task = remove_task(&active_list_head, msg.task_counter_id);

                    if (completed_task != NULL){

                        completed_task->task.completion_time = msg.completion_time;

                        if (completed_task->task.completion_time > msg.absolute_deadline) {
                            completed_task->next_task = overdue_list_head;
                            overdue_list_head = completed_task;

                        }else {
                                    completed_task->next_task = completed_list_head;
                                    completed_list_head = completed_task;
                        }
                    }
					update_tasks_priorities();

			// Checking if the Counts for the Current Tasks in the Active/Complete/Overdue
		    // are Required to Be Printed to the Console for Constant Monitoring
			}else if(msg.m_type == ACTIVE_TASKS) {
      			xQueueSend(lists_queue, &active_list_head, pdMS_TO_TICKS(0));
            }else if (msg.m_type == COMPLETED_TASKS) {
                xQueueSend(lists_queue, &completed_list_head, pdMS_TO_TICKS(0));
            }else if (msg.m_type == OVERDUE_TASKS) {
                xQueueSend(lists_queue, &overdue_list_head, pdMS_TO_TICKS(0));
            }
        }
    }
}

/*
 	 Temporary Structure Used for Communicating the Test Batch Task Info with Other Tasks
*/

typedef struct{

	uint8_t taskid;
    uint32_t task_counter_id;
    uint32_t release_time;
	uint16_t delay;
	uint16_t deadline;
	TaskHandle_t t_handle;

} task_params;

/*
    Task Handles to Control the Task Generator, Task Checker, and Monitor Functionality and
    Execution
*/

TaskHandle_t generator_handle;
TaskHandle_t monitor_handle;
TaskHandle_t dd_task_handle;

/*
    This Function Represents the Body for The Executing Task
    The Functionality is Simply a Very Short Delay that Simulates Execution Time

    Input: Task Parameters in Structure Type "task_params"
    Return: None
*/

void Execute_Task(void *pvParameters) {

	task_params *task_params_temp = (task_params *)pvParameters;

    uint32_t task_counter_id = task_params_temp->task_counter_id;
    uint32_t start_time = xTaskGetTickCount();
    uint32_t release_time = task_params_temp->release_time;
    uint32_t task_deadline = release_time + task_params_temp->deadline;
    uint16_t ticks_counter = 0;
    uint8_t task_id = task_params_temp->taskid;

    taskENTER_CRITICAL();

    printf("\nT:%i, Stat:%s, RT:%i, ST:%i, DT:%i, CT:%i\n",
                                    (int)task_id,
                                    "Active",
									(int)release_time,
                                    (int)0,
                                    (int)task_deadline,
                                    0);
    taskEXIT_CRITICAL();

    uint32_t endtime = xTaskGetTickCount();
    uint32_t prev_tick = xTaskGetTickCount();

    // Ensuring that the Number of Ticks Between Start Time and Finish Time is Always
    // Bigger or Equal to the Task Delay
    while(1 == 1) {

    	uint32_t tick = xTaskGetTickCount();

    	if (tick > prev_tick) ++ticks_counter;

    	prev_tick = tick;

    	// Ensuring the Total Delay is Accounting for the Delay of Printing the Task Stats
    	// in the Prior "Printf" Statement
    	if (ticks_counter >=  (task_params_temp->delay - (endtime - start_time))) break;
    };

    // Getting the Real Task Completion Time
    uint32_t delay = xTaskGetTickCount();

    //Flag The Task For Deletion After Completion
    delete_dd_task(task_counter_id, task_id, delay, task_deadline);

    taskENTER_CRITICAL();

    printf("\nT:%i, Stat:%s, RT:%i, ST:%i, DT:%i, CT:%i\n",
                                (int)task_id,
                                completed_overdue[task_deadline >= delay ? 0 : 1],
								(int)release_time,
                                (int)start_time,
                                (int)task_deadline,
                                (int)delay);

    taskEXIT_CRITICAL();

    // Delete the Task After Completion
    vTaskDelete(NULL);

    vPortFree(task_params_temp);
}

/*
    Temporary Queue Used to Determine which Task's Timer has Currently Finished,
    which ensures the Correct Test Task Parameters are Used for Periods, Execution Time, and
    Monitoring Tasks Executions
*/

QueueHandle_t timers_task_released;

/*
	Mutex Needed to Generate Tasks Based on Timers and Not Continuously
*/

SemaphoreHandle_t Mutex_Generator;

/*
    These Functions Times the Generation of New Task to ensure Periodic Tasks are Released
    After Specific Time Intervals

    Input: Timer Structure
    Return: None
*/

void vTask_1_Timer_Callback(TimerHandle_t xTimer) {

	uint8_t task = 1;
	xQueueSend(timers_task_released, &task, pdMS_TO_TICKS(QUEUE_DELAY));

	xSemaphoreGive(Mutex_Generator);

}

void vTask_2_Timer_Callback(TimerHandle_t xTimer) {

	uint8_t task = 2;
	xQueueSend(timers_task_released, &task, pdMS_TO_TICKS(QUEUE_DELAY));

	xSemaphoreGive(Mutex_Generator);

}

void vTask_3_Timer_Callback(TimerHandle_t xTimer) {

	uint8_t task = 3;
	xQueueSend(timers_task_released, &task, pdMS_TO_TICKS(QUEUE_DELAY));

	xSemaphoreGive(Mutex_Generator);
}

/*
	Helper Task the vDDTaskGenerator to Avoid Repetitive Ode when Generating Tasks at Time 0
	and Periodically After

	Input: Task ID (Between 1 and 3), Task Unique ID, Task Execution and Period Times Defined in the Test Batch
	Return: None
*/

void GenerateTasks(uint8_t task_id, uint32_t counter, uint16_t chosen_test_executions[], uint16_t chosen_test_periods[]){

	task_params *task_params_temp = pvPortMalloc(sizeof(task_params));

	task_params_temp->taskid = task_id;
	task_params_temp->task_counter_id = counter;
	task_params_temp->delay = chosen_test_executions[task_id - 1];
	task_params_temp->deadline = chosen_test_periods[task_id - 1];
	task_params_temp->release_time = xTaskGetTickCount();

	TaskHandle_t TaskHandle = NULL;

	xTaskCreate(Execute_Task, "task", 128, (void *) task_params_temp, 0, &TaskHandle);
	vTaskSuspend(TaskHandle);

	task_params_temp->t_handle = TaskHandle;

	create_dd_task(TaskHandle, PERIODIC, task_params_temp->task_counter_id, task_params_temp->taskid, task_params_temp->deadline);
}

/*
    This Function Generates New Task within Specific Time Intervals and
    Adds them to the Active Tasks List

    Input: None
    Return: None
*/

void vDDTaskGenerator(void *pvParameters) {

    Mutex_Generator = xSemaphoreCreateCounting(3,0);

    // Test Batches Parameters
    uint16_t test_1_periods[3] = {500, 500, 750};
    uint16_t test_2_periods[3] = {250, 500, 750};
    uint16_t test_3_periods[3] = {500, 500, 500};

    uint16_t test_1_executions[3] = {95, 150, 250};
    uint16_t test_2_executions[3] = {95, 150, 250};
    uint16_t test_3_executions[3] = {100, 200, 200};

    uint16_t chosen_test_periods[3];
    uint16_t chosen_test_executions[3];

    if (chosen_test_batch == 1) {
    	memcpy(chosen_test_periods, test_1_periods, sizeof(test_1_periods));
    	memcpy(chosen_test_executions, test_1_executions, sizeof(test_1_executions));
    }
    else if(chosen_test_batch == 2){
    	memcpy(chosen_test_periods, test_2_periods, sizeof(test_2_periods));
    	memcpy(chosen_test_executions, test_2_executions, sizeof(test_2_executions));
    }
    else if(chosen_test_batch == 3){
    	memcpy(chosen_test_periods, test_3_periods, sizeof(test_3_periods));
    	memcpy(chosen_test_executions, test_3_executions, sizeof(test_3_executions));
    }else{
    	perror("Input Correct Test Batch Number Between 1 and 3\n");
    	exit(EXIT_FAILURE);
    }

    timers_task_released = xQueueCreate(1, sizeof(uint8_t));

    // Task Counter ID
    uint32_t counter = 2;

 	// Tasks Timers Used to Periodically Generate Tasks and Add them to the Tasks Queue
    TimerHandle_t xTimer_1;
    TimerHandle_t xTimer_2;
    TimerHandle_t xTimer_3;

    xTimer_1 = xTimerCreate("Task_1_Timer", pdMS_TO_TICKS(chosen_test_periods[0]), pdTRUE, NULL, vTask_1_Timer_Callback);
    xTimer_2 = xTimerCreate("Task_2_Timer", pdMS_TO_TICKS(chosen_test_periods[1]), pdTRUE, NULL, vTask_2_Timer_Callback);
    xTimer_3 = xTimerCreate("Task_3_Timer", pdMS_TO_TICKS(chosen_test_periods[2]), pdTRUE, NULL, vTask_3_Timer_Callback);

    GenerateTasks(1, 0, chosen_test_executions, chosen_test_periods);
    GenerateTasks(2, 1, chosen_test_executions, chosen_test_periods);
    GenerateTasks(3, 2, chosen_test_executions, chosen_test_periods);

    xTimerStart(xTimer_1, 0);
    xTimerStart(xTimer_2, 0);
    xTimerStart(xTimer_3, 0);

    uint8_t task_id;

    while (1) {

    	if (xSemaphoreTake(Mutex_Generator, pdMS_TO_TICKS(SEMAPHORE_DELAY)) == pdTRUE){

    		// Ensuring the Task is Never Preempted when Adding  newly Generated Task to the Tasks Queue,
    		// Avoiding Task Ordering Problems
            taskENTER_CRITICAL();

			++counter;

			xQueueReceive(timers_task_released, &task_id, pdMS_TO_TICKS(QUEUE_DELAY));

			GenerateTasks(task_id, counter, chosen_test_executions, chosen_test_periods);

			// Resuming Preemption
            taskEXIT_CRITICAL();
    	}
    }
}

/*
    Mutexes that Ensure Printing Messages Don't Happen Until Previous Ones Finish Executing,
    Minimizing Overlapping Between Statements and Printing Total Tasks Counts 200 Milliseconds
*/

SemaphoreHandle_t Mutex_Monitor_Timer = NULL;

/*
    This Functions Times the Print Statement for the Total Tasks Counts in Active, Overdue and Completed States

    Input: Timer Structure
    Return: None
*/

void vTotal_Counts_Timer_Callback(TimerHandle_t xTimer) {

	xSemaphoreGive(Mutex_Monitor_Timer);
}

/*
    This Function Prints Out the Current Status of Tasks in Real-time

    Input: None
    Output: None
*/

void vMonitorTask(void *pvParameters) {

	Mutex_Monitor_Timer = xSemaphoreCreateBinary();

    // Timer is Used to Report the Total Number of Tasks Every 200 Milliseconds
    TimerHandle_t xTimer_Total_Counts = xTimerCreate("Total_Counts_Timer", pdMS_TO_TICKS(200),
                                                     pdTRUE, NULL, vTotal_Counts_Timer_Callback);

    xTimerStart(xTimer_Total_Counts, 0);


    while (1) {

    	//Checking the Timer has Been Triggered
    	if (xSemaphoreTake(Mutex_Monitor_Timer, pdMS_TO_TICKS(SEMAPHORE_DELAY)) == pdTRUE){

    		printf("\n");
			get_current_tasks(get_active_dd_task_list(), "GET_ACTIVE_TASK_COUNT");
			get_current_tasks(get_completed_dd_task_list(), "GET_COMPLETED_TASK_COUNT");
			get_current_tasks(get_overdue_dd_task_list(), "GET_OVERDUE_TASK_COUNT" );

    	}

    }
}

/*
    This is the Main Function, It Specified the Tasks Needed for Scheduling and Initiates
    the FreeRtos Scheduler

    Input: None
    Output: None
*/

int main(void) {

    tasks_queue = xQueueCreate(10, sizeof(task_message));
    lists_queue = xQueueCreate(10, sizeof(struct dd_task_list));

    if (tasks_queue != NULL && lists_queue != NULL)
    {

    	printf("\nTasks Monitor:\n");

    	//Defining and Initiating the Tasks for FreeRTOS
    	xTaskCreate(vDDTaskGenerator, "Task_Generator", 256, NULL, MAX_PRIORITY, &generator_handle);
		xTaskCreate(vDDSTask, "Task_Checker", 128, NULL, MAX_PRIORITY, &dd_task_handle);
		xTaskCreate(vMonitorTask, "Task_Monitor", 128, NULL, MAX_PRIORITY, &monitor_handle);

		vTaskStartScheduler();
    }

    while(1);

    return 0;
}

/*-----------------------------------------------------------*/

void vApplicationMallocFailedHook( void )
{
	/* The malloc failed hook is enabled by setting
	configUSE_MALLOC_FAILED_HOOK to 1 in FreeRTOSConfig.h.

	Called if a call to pvPortMalloc() fails because there is insufficient
	free memory available in the FreeRTOS heap.  pvPortMalloc() is called
	internally by FreeRTOS API functions that create tasks, queues, software
	timers, and semaphores.  The size of the FreeRTOS heap is set by the
	configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( xTaskHandle pxTask, signed char *pcTaskName )
{
	( void ) pcTaskName;
	( void ) pxTask;

	/* Run time stack overflow checking is performed if
	configconfigCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
	function is called if a stack overflow is detected.  pxCurrentTCB can be
	inspected in the debugger if the task name passed into this function is
	corrupt. */
	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook( void )
{
volatile size_t xFreeStackSpace;

	/* The idle task hook is enabled by setting configUSE_IDLE_HOOK to 1 in
	FreeRTOSConfig.h.

	This function is called on each cycle of the idle task.  In this case it
	does nothing useful, other than report the amount of FreeRTOS heap that
	remains unallocated. */
	xFreeStackSpace = xPortGetFreeHeapSize();

	if( xFreeStackSpace > 100 )
	{
		/* By now, the kernel has allocated everything it is going to, so
		if there is a lot of heap remaining unallocated then
		the value of configTOTAL_HEAP_SIZE in FreeRTOSConfig.h can be
		reduced accordingly. */
	}
}
/*-----------------------------------------------------------*/

static void prvSetupHardware( void )
{
	/* Ensure all priority bits are assigned as preemption priority bits.
	http://www.freertos.org/RTOS-Cortex-M3-M4.html */
	NVIC_SetPriorityGrouping( 0 );

	/* TODO: Setup the clocks, etc. here, if they were not configured before
	main() was called. */
}
