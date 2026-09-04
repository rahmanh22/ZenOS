<div align="center">

# ⚡ ZenOS

**سیستم‌عامل بلادرنگ برای ARM Cortex-M — نوشته‌شده با C++11.**

### **سادگی — امنیت — سرعت**

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

## 🧘 درباره پروژه

کلمه *Zen* به معنای شفافیت از طریق سادگی است — کنار گذاشتن زواید تا فقط آنچه اهمیت دارد باقی بماند. ایده پشت این سیستم‌عامل همین است.

ZenOS کارهای سخت سیستم‌های تعبیه‌شده — زمان‌بندی، همگام‌سازی، حفاظت حافظه، بازیابی خطا — را خودش انجام می‌دهد تا شما روی برنامه‌تان تمرکز کنید. چیزهایی که معمولاً ده‌ها خط کد راه‌اندازی می‌خواهند اینجا به یک فراخوانی تابع خلاصه شده‌اند. سیستم‌عامل خودش بقیه را مدیریت می‌کند.

برای ARM Cortex-M (M3, M4, M7) روی STM32 ساخته شده و مکانیسم‌های ایمنی متعددی به‌صورت پیش‌فرض دارد: بررسی پشته، محافظت MPU، تایمرها، تأیید CRC و پایش ددلاین. اگر محصول پزشکی یا صنعتی می‌سازید، می‌توانید انطباق IEC 62304 یا IEC 61508 را در زمان کامپایل اجرا کنید — خود کامپایلر به شما می‌گوید چیزی کم است.

> **بدون Heap. بدون تخصیص‌های پنهان. بدون سورپرایز.**

### ✨ ویژگی‌ها

| ویژگی | جزئیات |
|:------|:-------|
| 🧩 اصول IPC | تسک‌ها، قفل‌ها، eventها، صفوف، سیگنال‌ها — همه با حداقل کد اضافه |
| ⬆️ سقف اولویت | پروتکل جلوگیری از وارونگی اولویت در سیستم‌های بلادرنگ |
| 🏥 IEC 62304 | اجرای ایمنی در زمان کامپایل برای محصولات پزشکی |
| 🏭 IEC 61508 | اجرای ایمنی در زمان کامپایل برای محصولات صنعتی |
| 🧪 تست‌شده | روی سخت‌افزار واقعی (STM32F103C8T6) با بیش از 22 مجموعه تست |
| 🔒 صفر هزینه | قالب‌های C++ و RAII — انتزاع بدون هزینه اجرا |

### ⚡ چرا ZenOS؟

| مزیت | ZenOS | FreeRTOS | Zephyr |
|:-----|:------|:---------|:-------|
| **تیک** | **100μs** (۱۰ برابر دقیق‌تر) | 1ms | 10ms |
| **زمان‌بند** | **O(1) همیشه** | O(n) در بدترین حالت | O(n) در هر اولویت |
| **Heap** | **هرگز (قطعی)** | موجود (پرخطر) | موجود |
| **ایمنی** | **داخلی (canary، MPU، watchdog، RAM test، CRC)** | بسته جداگانه | بسته جداگانه |
| **IEC 62304/61508** | **اجرا در زمان کامپایل** | بدون | بدون |
| **تسک‌های دوره‌ای** | **اولی (نه timer)** | از timer نرم‌افزاری | از timer نرم‌افزاری |
| **سقف IPC** | **Immediate Priority Ceiling** | فقط Priority Inheritance | بدون |
| **C++ RAII** | **پشتیبانی کامل** | بدون | جزئی |

> 📖 [مقایسه فنی کامل → ZENOS_ADVANTAGES.md](ZENOS_ADVANTAGES.md)

### 🖥️ خانواده‌های پشتیبانی‌شده

```
 STM32F1  STM32F2  STM32F4  STM32F7  STM32H7
  M3        M3       M4       M7      Dual M7+M4

 STM32G0  STM32G4  STM32L0  STM32L4  STM32WB
  M0+       M4       M0+       M4       M4
```

---

## 🚀 شروع سریع

یک برنامه کامل سیستم‌عامل بلادرنگ در کمتر از 20 خط:

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

> 💡 همین. زمان‌بند از `os_start()` کنترل را به دست می‌گیرد و هرگز برنمی‌گردد. تسک شما هر 500ms اجرا می‌شود، LED چشمک می‌زند، و شما حتی یک خط کد زمان‌بندی هم ننوشتید.

### 🏗️ معماری

```
┌───────────────────────────────────────────────────┐
│                 تسک‌های برنامه                     │
├───────────────────────────────────────────────────┤
│             هسته ZenOS (C++11)                    │
│  ┌────────────┬─────────────┬──────────────────┐  │
│  │  زمان‌بند   │     IPC     │  لایه ایمنی     │  │
│  │  O(1)      │  eventها   │  canary پشته    │  │
│  │  صف        │  قفل‌ها     │  محافظت MPU     │  │
│  │  اولویت    │  صفوف       │  تایمر سخت/نرم  │  │
│  │            │  سیگنال‌ها  │  آزمون RAM      │  │
│  │            │             │  بررسی CRC      │  │
│  │            │             │  پایش ددلاین    │  │
│  └────────────┴─────────────┴──────────────────┘  │
├───────────────────────────────────────────────────┤
│            HAL STM32 (تولیدشده توسط CubeMX)       │
├───────────────────────────────────────────────────┤
│         سخت‌افزار ARM Cortex-M (M3/M4/M7)         │
└───────────────────────────────────────────────────┘
```

راهنمای کامل API → [API_TUTORIAL_FA.md](API_TUTORIAL_FA.md) | [English](API_TUTORIAL.md)

---

## 📚 مستندات

| سند | محتوا |
|:----|:------|
| 📖 [SAFETY_MANUAL_FA.md](SAFETY_MANUAL_FA.md) | معماری ایمنی، محدودیت‌ها، جزئیات انطباق با استانداردها |
| 📘 [API_TUTORIAL_FA.md](API_TUTORIAL_FA.md) | راهنمای کامل API با مثال‌ها (فارسی) |
| 📗 [API_TUTORIAL.md](API_TUTORIAL.md) | راهنمای کامل API با مثال‌ها (انگلیسی) |
| 🤝 [CONTRIBUTING_FA.md](CONTRIBUTING_FA.md) | نحوه مشارکت |
| ⚡ [ZENOS_ADVANTAGES.md](ZENOS_ADVANTAGES.md) | مقایسه فنی با FreeRTOS، Zephyr، RT-Thread |
| ⚙️ [ZenOS_Config.hpp](ZenOS/ZenOS/ZenOS_Config.hpp) | هر گزینه پیکربندی با یادداشت‌های ایمنی |

---

## 🛡️ ویژگی‌های ایمنی

تمام مکانیسم‌ها به‌صورت **پیش‌فرض فعال** هستند و در `ZenOS_Config.hpp` قابل تنظیم‌اند.

| مکانیسم | ماکرو پیکربندی | عملکرد |
|:--------|:---------------|:-------|
| 🔍 canary پشته | `OS_SAFETY_SOFT_WATCHDOG` | بررسی canary + محدوده SP برای هر تسک |
| 🔐 یکپارچگی TCB | `OS_MONITOR_TCB_INTEGRITY` | اعتبارسنجی عدد جادو برای خرابی حافظه |
| ⏱️ پایش ددلاین | `OS_MONITOR_DEADLINE` | تشخیص و واکنش به تأخیر ددلاین تسک‌ها |
| 📋 ثبت خطا | `OS_MONITOR_ERROR_LOG` | بافر حلقه‌ای با زمان، شناسه تسک و شدت |
| 🐕 تایمر سخت‌افزاری | `OS_SAFETY_HW_WATCHDOG` | ادغام IWDG STM32 با تغذیه مشروط |
| 💤 تایمر نرم‌افزاری | `OS_SAFETY_SOFT_WATCHDOG` | تشخیص تسک‌های گیر کرده در مهلت پیکربندی |
| 🧪 آزمون RAM | `OS_SAFETY_RAM_TEST` | March C- پس‌زمینه برای یکپارچگی SRAM |
| ✅ بررسی CRC | `OS_SAFETY_CRC_CHECK` | یکپارچگی فلش از طریق مدها CRC STM32 |
| 🛡️ MPU | `OS_SAFETY_MPU` | حفاظت حافظه هر تسک (Cortex-M3+) |

---

## 💰 حمایت از ZenOS

این یک پروژه تک‌نفره است. من آن را می‌سازم، تست می‌کنم، مستندات می‌نویسم و نگهداری می‌کنم — همه در اوقات فراغت، بدون بودجه. اگر ZenOS برایتان مفید بوده، چه برای یادگیری، چه نمونه‌سازی و چه تولید محصول، از حمایت مالی استقبال می‌کنم.

### 🎯 پول به کجا می‌رود

<table>
<tr>
<td align="center" width="25%">

### 🏥 گواهینامه
انطباق IEC 62304 و IEC 61508 ارزان نیست. مستندات رسمی، ردیابی، ممیزی — همه هزینه دارند.

</td>
<td align="center" width="25%">

### 🔧 سخت‌افزار
بردهای STM32 جدید، تحلیل‌گرهای منطقی، پروب‌های JTAG — هر خانواده به تجهیزات تست خودش نیاز دارد.

</td>
<td align="center" width="25%">

### ☁️ زیرساخت
خطوط CI، کلاود‌بیلدها، اجرای خودکار تست‌ها. نگهداری همه اینها منابع می‌خواهد.

</td>
<td align="center" width="25%">

### 📖 مستندات و جامعه
نوشتن مستندات خوب زمان‌بر است. پاسخ به Issues زمان‌بر است. هر دو مهم هستند.

</td>
</tr>
</table>

## 💸 کمک مالی

حمایت مالی شما مستقیماً توسعه، تست، گواهینامه و مستندات را تأمین می‌کند.

<br>

<table>
<tr>
<td align="center" width="50%">

<sup>

**🟠 بیت‌کوین (BTC)**

</sup>

<code style="font-size: 1.4em; font-weight: bold; color: #f7931a;  ">

`bc1qd39vgmnweuzh5hp2cqm4cnh782xga6wph3v650`

</code>

</td>
<td align="center" width="50%">

<sup>

**🔵 اتریوم (ETH) / تتر (USDT - ERC-20)**

</sup>

<code style="font-size: 1.4em; font-weight: bold; color: #f7931a;  ">

`0x1C21c39324F65a38Fb8de9ccB92aB01FdeD1534C`

</code>

</td>
</tr>
</table>


<div dir="rtl">

> 📩 **پس از انجام پرداخت، لطفاً آدرس ایمیل یا شناسه تراکنش خود را به [rahman.h22@gmail.com](mailto:rahman.h22@gmail.com) ارسال کنید تا شخصاً از شما قدردانی شود.** 

</div>

### 🌟 راه‌های دیگر کمک

<table>
<tr>
<td align="center">⭐<br><b>ستاره</b><br>بدهید</td>
<td align="center">🐛<br><b>باگ</b><br>گزارش کنید</td>
<td align="center">📝<br><b>آموزش</b><br>بنویسید</td>
<td align="center">🗣️<br><b>معرفی</b><br>کنید</td>
</tr>
</table>

---

## 📬 ارتباط با ما

<div dir="rtl">

**گروه علمی و تحقیقاتی رایمون** — Rahman Heidari

📧 ایمیل: [rahman.h22@gmail.com](mailto:rahman.h22@gmail.com)

برای سؤالات، پیشنهادات یا فرصت‌های همکاری، با ما در تماس باشید.

</div>

---

<div align="center">

<br>

**با ❤️ برای جامعه تعبیه‌شده ساخته شده**

[![GitHub stars](https://img.shields.io/github/stars/rahmanheidari/ZenOS?style=social)]()

<br>

*ZenOS — (سادگی — امنیت — سرعت)*

</div>
