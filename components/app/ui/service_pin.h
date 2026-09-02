#pragma once

/* ------------------------------------------------------------------
 *  Service PIN — gates the Maintenance sections that can take the
 *  vehicle out of service.
 *
 *  Background: a customer wandered into Bus Setup and HMI Role and
 *  changed both. Bus Setup reprograms the baud rate of every slave on
 *  the RS-485 bus, and HMI Role changes the master/secondary topology
 *  and drops the wireless pairing -- between them they silenced the
 *  whole bus, and recovering it needed a scanner sketch and a laptop.
 *  Those screens are installer tools, not customer settings.
 *
 *  >>> CHANGE THIS BEFORE SHIPPING <<<
 *  It is a compile-time constant on purpose:
 *    - deliberately NOT editable from the touchscreen, so a customer
 *      cannot change it and lock the installer out;
 *    - it lives in the firmware you build, so you cannot forget it --
 *      it is always recoverable by reading this file.
 *  Changing it means a reflash, which is the right trade for a
 *  manufacturer who controls the firmware anyway.
 *
 *  Digits only (the unlock screen uses a numeric keypad), 4-8 of them.
 * ------------------------------------------------------------------ */
#define SERVICE_PIN  "2580"

/* How the gate behaves:
 *  - unlocking applies to the whole Maintenance visit, so an installer
 *    types it once and can move between locked sections freely;
 *  - it re-locks on leaving the Maintenance tab, so the next person to
 *    wander in starts locked again. */
