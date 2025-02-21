The many hoops to jump through and how it's done
================================================

Legacy RISC OS code often enables interrupts while in SVC mode.

SWIs you call can call Vectors and Service Calls whose handlers can do pretty much anything.

There's an IRQ semaphore I still don't know what it does.

There a Currently Active Object word, ditto.


The underlying kernel
=====================

Does not like SVC code being interrupted!

The model is that SWI calls get routed to either very short code or into a queue that a user mode task will process and resume the caller at some later time. Data or prefetch (instruction) aborts similarly.

To that end, there are small stacks on each core for the abt, svc, irq, etc. modes that are sufficient to schedule a task or tasks to deal with the event, or deal with them immediately.


Handling the conflict - the Legacy subsystem
============================================

The startup code initialises various areas of memory (zero page, heaps, and MOS workspace) expected by Legacy code (and creates a RamFS DA because the module is incapable of managing its own memory).

Sprite Extend workspace and Desktop workspace (at 0xff000000), too.

A task (`centiseconds`) runs that sleeps for 9 (ms) ticks and triggers another task `do_TickOne` to call the legacy routine `TickOne` essentially on the tenth. There's a good chance that TickOne will call legacy code needing the legacy stack, so the two tasks are decoupled for about half a second before problems begin.

The Legacy subsystem creates a single, shared, relatively large, legacy SVC stack. One task at a time may have control of the stack, but if it gets blocked (reading from a file, for example), other tasks can use the remaining stack when calling legacy SWIs.

Interrupts in privileged modes
------------------------------

A concession to the need to deal with legacy code is that the OSTask subsystem, detecting an interrupt that occurs other than in USR mode will call `interrupting_privileged_code`. TODO: There should be a compile time switch to turn it off.

In that case, the current SVC stack pointer is stored and the blocked task manipulated so that when it's re-started, it will go back to using the legacy stack.

** No new software should ever enable interrupts in a privileged mode! **

How it works
------------

Legacy code will inevitably call, probably indirectly, modern code that will block the running task. This is similar to the IRQ problem, but more complicated.

The Legacy subsystem provides the `execute_swi` routine that intercepts all SWI calls.

When the current legacy task calls a legacy SWI, the SWI code is executed (but see BlockedLegacy, below), all other tasks calling legacy SWIs are **queued**.

At startup, a task `serve_legacy_swis` (in Legacy/user.c) is spawned that does nothing more than wait at the queue and call `XOS_CallASWIR12` with the result. (The Legacy subsystem detects when it's not a real `XOS_CallASWIR12` by checking the calling task handle.) This task acts as a gatekeeper for the legacy SVC stack.

While a SWI is being executed, the `serve_legacy_swis` task is paused until the SWI returns or the current legacy task is blocked.

BlockedLegacy
-------------

The BlockedLegacy return address is used as a marker for tasks that may be blocked during the legacy SWI that's being executed. It replaces the true return address for the task's SWI call.

The idea was that it would never get called, but it does (by the Wimp, which thinks it's returning directly from a recursive Wimp SWI).

It's because the Wimp SWI caller is the current legacy task....

So:
    usr32 Wimp SWI
        -> `OS_CallAVector`
            -> handler (SVC mode?)
                -> Wimp SWI (treated as recursing)
                return to BlockedLegacy.


