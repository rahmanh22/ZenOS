<div align="center">

<img src="ZenOS_logo.svg" alt="ZenOS Logo" width="400" />

**RT operating system for ARM Cortex-M — written in C++11.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg?style=for-the-badge)](LICENSE)
[![Version](https://img.shields.io/badge/Version-1.0.0-green.svg?style=for-the-badge)]()
[![Platform](https://img.shields.io/badge/Platform-ARM%20Cortex--M-orange.svg?style=for-the-badge)]()
[![Language](https://img.shields.io/badge/Language-C%2B%2B11-purple.svg?style=for-the-badge)]()
[![Standard](https://img.shields.io/badge/IEC-62304-red.svg?style=for-the-badge)]()
[![Standard](https://img.shields.io/badge/IEC-61508-red.svg?style=for-the-badge)]()

<br>

[**🇮🇷 فارسی**](README_FA.md) | [**🇬🇧 English**](README.md)

<br>


</div>

---

## 🧘 About

The word *Zen* means clarity through simplicity — stripping away the unnecessary until only what matters remains. That's the idea behind this RTOS.

ZenOS handles the hard parts of embedded systems — scheduling, synchronization, memory protection, fault recovery — so you can focus on your actual application. Most things that would normally take dozens of lines of setup code are reduced to a single function call. The kernel figures out the rest.

It's built for ARM Cortex-M (M3, M4, M7) on STM32, and it carries a set of safety mechanisms out of the box: stack checking, MPU protection, watchdogs, CRC verification, and deadline monitoring. If you're targeting medical or industrial products, you can enforce IEC 62304 or IEC 61508 compliance at compile time — the build itself will tell you if something's missing.

> **No heap. No hidden allocations. No surprises.**

### ✨ Highlights

| Feature | Details |
|:--------|:--------|
| 🧩 IPC Primitives | Tasks, mutexes, events, queues, semaphores — all with minimal boilerplate |
| ⬆️ Priority Ceiling | Protocol to prevent priority inversion in real-time systems |
| 🏥 IEC 62304 | Compile-time safety enforcement for medical devices |
| 🏭 IEC 61508 | Compile-time safety enforcement for industrial systems |
| 🧪 Tested | On real hardware (STM32F103C8T6) with 22+ test suites |
| 🔒 Zero Overhead | C++ templates and RAII — abstraction without runtime cost |

### ⚡ Why ZenOS?

| Advantage | ZenOS | FreeRTOS | Zephyr |
|:----------|:------|:---------|:-------|
| **Tick resolution** | **100μs** (10× finer) | 1ms | 10ms |
| **Scheduler** | **O(1) always** | O(n) worst case | O(n) within priority |
| **Heap allocation** | **Never (deterministic)** | Available (risky) | Available |
| **Safety built-in** | **Yes (canary, MPU, watchdog, RAM test, CRC)** | Optional package | Optional package |
| **IEC 62304/61508** | **Compile-time enforcement** | None | None |
| **Periodic tasks** | **First-class (not timers)** | Use software timers | Use software timers |
| **IPC ceiling** | **Immediate Priority Ceiling** | Priority inheritance only | None |
| **C++ RAII** | **Full support** | None | Partial |

> 📖 [Full technical comparison → ZENOS_ADVANTAGES.md](ZENOS_ADVANTAGES.md)

### 🖥️ Supported Families

```
 STM32F1  STM32F2  STM32F4  STM32F7  STM32H7
  M3        M3       M4       M7      Dual M7+M4

 STM32G0  STM32G4  STM32L0  STM32L4  STM32WB
  M0+       M4       M0+       M4       M4
```

---

## 🚀 Quick Start

A complete RTOS application in under 20 lines:

```cpp
#include "ZenOS.hpp"

void task_blink(void) {
    while (1) {
        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
        os_delay_ms(500);
    }
}

int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();

    os_init();
    os_task_create(task_blink, 1);
    os_start();
}
```

> 💡 That's it. The kernel takes over from `os_start()` and never returns. Your task runs every 500ms, the LED blinks, and you didn't have to write a single line of scheduling code.

### 🏗️ Architecture

```
┌───────────────────────────────────────────────────┐
│                Application Tasks                  │
├───────────────────────────────────────────────────┤
│              ZenOS Kernel (C++11)                 │
│  ┌────────────┬─────────────┬──────────────────┐  │
│  │  Scheduler │     IPC     │  Safety Layer    │  │
│  │  O(1)      │  Events     │  Stack Canary    │  │
│  │  priority  │  Mutexes    │  MPU Protection  │  │
│  │  queue     │  Queues     │  HW/SW Watchdog  │  │
│  │            │  Semaphores │  RAM Test (C-)   │  │
│  │            │             │  CRC ROM Check   │  │
│  │            │             │  Deadline Mon.   │  │
│  └────────────┴─────────────┴──────────────────┘  │
├───────────────────────────────────────────────────┤
│           STM32 HAL (CubeMX-generated)            │
├───────────────────────────────────────────────────┤
│        ARM Cortex-M Hardware (M3/M4/M7)           │
└───────────────────────────────────────────────────┘
```

For the full API guide → [API_TUTORIAL.md](API_TUTORIAL.md) | [راهنمای فارسی](API_TUTORIAL_FA.md)

---

## 📚 Documentation

| Document | What's in it |
|:---------|:-------------|
| 📖 [SAFETY_MANUAL.md](SAFETY_MANUAL.md) | Safety architecture, limitations, IEC compliance details |
| 📘 [API_TUTORIAL.md](API_TUTORIAL.md) | Complete API guide with examples (English) |
| 📗 [API_TUTORIAL_FA.md](API_TUTORIAL_FA.md) | Complete API guide with examples (Persian) |
| 🤝 [CONTRIBUTING.md](CONTRIBUTING.md) | How to contribute |
| ⚡ [ZENOS_ADVANTAGES.md](ZENOS_ADVANTAGES.md) | Technical comparison with FreeRTOS, Zephyr, RT-Thread |
| ⚙️ [ZenOS_Config.hpp](ZenOS/ZenOS/ZenOS_Config.hpp) | Every config option with safety annotations |

---

## 🛡️ Safety Features

All mechanisms are **enabled by default** and can be individually toggled in `ZenOS_Config.hpp`.

| Mechanism | Config Macro | What it does |
|:----------|:-------------|:-------------|
| 🔍 Stack Canary | `OS_SAFETY_SOFT_WATCHDOG` | Canary + SP bounds checking per task |
| 🔐 TCB Integrity | `OS_MONITOR_TCB_INTEGRITY` | Magic number validation for memory corruption |
| ⏱️ Deadline Monitor | `OS_MONITOR_DEADLINE` | Detects and reacts to task deadline misses |
| 📋 Error Log | `OS_MONITOR_ERROR_LOG` | Ring buffer with timestamp, task ID, severity |
| 🐕 HW Watchdog | `OS_SAFETY_HW_WATCHDOG` | STM32 IWDG integration with conditional feed |
| 💤 SW Watchdog | `OS_SAFETY_SOFT_WATCHDOG` | Detects stuck tasks within configurable timeout |
| 🧪 RAM Test | `OS_SAFETY_RAM_TEST` | Background March C- for SRAM integrity |
| ✅ CRC Check | `OS_SAFETY_CRC_CHECK` | Flash integrity via STM32 CRC peripheral |
| 🛡️ MPU | `OS_SAFETY_MPU` | Per-task memory protection (Cortex-M3+) |

---

## 💰 Support ZenOS

This is a solo project. I build it, test it, write the docs, and maintain it — all in my spare time, without funding. If you find ZenOS useful, whether for learning, prototyping, or shipping a product, consider supporting its continued development.

### 🎯 Where the money goes

<table>
<tr>
<td align="center" width="25%">

### 🏥 Certification
IEC 62304 and IEC 61508 compliance isn't cheap. Formal documentation, traceability, audits — it all costs real money.

</td>
<td align="center" width="25%">

### 🔧 Hardware
New STM32 boards, logic analyzers, JTAG probes — every supported family needs its own test setup.

</td>
<td align="center" width="25%">

### ☁️ Infrastructure
CI pipelines, cloud builds, automated test runs. Keeping everything running takes resources.

</td>
<td align="center" width="25%">

### 📖 Docs & Community
Writing good documentation takes time. Answering issues takes time. Both matter.

</td>
</tr>
</table>

## 💸 Donate

Your donation directly funds development, testing, certification, and documentation.

<br>

<table>
<tr>
<td align="center">

🟠 **Bitcoin (BTC)**

`bc1qd39vgmnweuzh5hp2cqm4cnh782xga6wph3v650`

</td>
</tr>
<tr>
<td align="center">

🔵 **Ethereum (ETH) / USDT (ERC-20)**

`0x1C21c39324F65a38Fb8de9ccB92aB01FdeD1534C`

</td>
</tr>
</table>

<br>

> ☕ Even a few dollars helps. If everyone who cloned this repo bought me a coffee, I could afford a proper test bench for every STM32 family and hire someone to help with the certification paperwork.

> 📩 **After making a donation, please send an email to [rahman.h22@gmail.com](mailto:rahman.h22@gmail.com) with your transaction ID or wallet address so I can personally thank you.** 

### 🌟 Other ways to help

<table>
<tr>
<td align="center">⭐<br><b>Star</b><br>the repo</td>
<td align="center">🐛<br><b>Report</b><br>a bug</td>
<td align="center">📝<br><b>Write</b><br>a tutorial</td>
<td align="center">🗣️<br><b>Tell</b><br>someone</td>
</tr>
</table>

---

## 📬 Contact

**Raymon Research Team** — Rahman Heidari

📧 Email: [rahman.h22@gmail.com](mailto:rahman.h22@gmail.com)

For questions, suggestions, or collaboration opportunities, feel free to reach out. I'm always happy to hear from developers using ZenOS in their projects.

---

<div align="center">

<br>

**Built with ❤️ for the embedded community**

[![GitHub stars](https://img.shields.io/github/stars/rahmanh22/ZenOS?style=social)]()

<br>

*ZenOS — (Simplicity , Security , Speed)*

</div>
