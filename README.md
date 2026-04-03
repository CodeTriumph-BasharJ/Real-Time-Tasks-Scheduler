# EDF Scheduler Project

This project illustrates an **EDF (Earliest-Deadline-First) scheduler** that sorts tasks' execution based on their deadlines in ascending order. 

The task with the **lowest deadline** has the **highest priority** and is executed first, or preempts the currently executing task with a lower priority (higher deadline).

The project is built on top of the **FreeRTOS Round-Robin Scheduler**, where each execution is timed based on the default implementation within the FreeRTOS internal configuration.

The project significantly utilizes techniques that **protect critical sections** that are continuously accessible, ensuring **data integrity** by avoiding simultaneous data modifications.

The final results of the scheduler are highly accurate and match the expected outcomes associated with the provided test batches. Furthermore, the **maximum delay** noted was a minimal **2 milliseconds** above the desired completion time for tested tasks.
