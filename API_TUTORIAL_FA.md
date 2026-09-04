# آموزش جامع API سیستم‌عامل بلادرنگ ZenOS

**نسخه:** 1.0.0 | **پلتفرم:** ARM Cortex-M (STM32) | **زبان:** C++11 / C

---

## فهرست مطالب

1. [شروع سریع](#۱-شروع-سریع)
2. [مدیریت تسک‌ها](#۲-مدیریت-تسک‌ها)
3. [زمان‌بندی و تأخیرها](#۳-زمان‌بندی-و-تأخیرها)
4. [بخش‌های حیاتی (OS_SAFE)](#۴-بخش‌های-حیاتی-os_safe)
5. [رویدادها (OS_EVENT)](#۵-رویدادها-os_event)
6. [قفل‌ها (OS_MUTEX)](#۶-قفل‌ها-os_mutex)
7. [صفوف (OS_QUEUE)](#۷-صفوف-os_queue)
8. [سیگنال‌ها (OS_SEMAPHORE)](#۸-سیگنال‌ها-os_semaphore)
9. [تسک‌های دوره‌ای — روش ZenOS (بدون تایمر نرم‌افزاری)](#۹-تسک‌های-دوره‌ای--روش-zenos-بدون-تایمر-نرم‌افزاری)
10. [مدیریت خطا و پایش](#۱۰-مدیریت-خطا-و-پایش)
11. [ویژگی‌های ایمنی](#۱۱-ویژگی‌های-ایمنی)
12. [نکات و ترفندهای پیشرفته](#۱۲-نکات-و-ترفندهای-پیشرفته)
13. [کتابخانه الگوهای رایج](#۱۳-کتابخانه-الگوهای-رایج)

---

## ۱. شروع سریع

### ۱.۱ اضافه کردن و راه‌اندازی

```cpp
#include "ZenOS.hpp"

int main() {
    HAL_Init();
    SystemClock_Config();
    // ... راه‌اندازی مدها (UART، GPIO و ...) ...

    os_init();  // راه‌اندازی اجزای داخلی سیستم‌عامل

    // ایجاد تسک‌ها قبل از os_start()
    os_task_create(task_main, 5);            // اولویت 5، پشته پیش‌فرض
    os_task_create(task_sensor, 3, 100);     // اولویت 3، دوره‌ای 100 میلی‌ثانیه
    os_task_create_st(task_ui, 8, 0, 1024); // اولویت 8، پشته سفارشی 1KB

    os_start();  // هرگز برنمی‌گردد — زمان‌بند همه‌چیز را کنترل می‌کند
}
```

### ۱.۲ حداقل تسک مورد نیاز

```cpp
void task_blink(void) {
    while (1) {
        HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
        os_delay_ms(500);  // تأخیر 500 میلی‌ثانیه و واگذاری CPU
    }
}
```

> **💡 نکته:** هر تسکی باید شامل `while(1)` باشد و باید به‌صورت دوره‌ای واگذاری کند (از طریق `os_delay_ms`، `os_yield` یا انتظار روی IPC). تسکی که هرگز واگذاری نکند توسط تایمر نرم‌افزاری ایمنی شناسایی و ریست می‌شود.

### ۱.۳ ترتیب راه‌اندازی

```
os_init()  →  ایجاد تسک‌ها  →  os_start()  →  زمان‌بند تا ابد اجرا می‌کند
    ↑                                                        ↓
    └──────── os_start() هرگز برنمی‌گردد ─────────────────────┘
```

**قانون حیاتی:** تمام تسک‌ها باید **قبل از** `os_start()` ایجاد شوند. ایجاد تسک بعد از `os_start()` مقدار -1 برمی‌گرداند و `TASK_AFTER_START` گزارش می‌کند.

---

## ۲. مدیریت تسک‌ها

### ۲.۱ ایجاد تسک

```cpp
// اندازه پشته پیش‌فرض (OS_KERNEL_STACK_SIZE = 512 بایت)
os_task_create(task_function, priority);              // اولویت 1-255
os_task_create(task_function, priority, period_ms);   // + دوره‌ای

// اندازه پشته سفارشی (به بایت)
os_task_create_st(task_function, priority, period_ms, stack_bytes);
```

**پارامترها:**
| پارامتر | نوع | توضیح |
|----------|------|--------|
| `entry` | `void(*)(void)` | اشاره‌گر تابع تسک (به‌عنوان شناسه یکتا استفاده می‌شود) |
| `priority` | `uint8_t` | 1 = کمترین، 255 = بیشترین. 0 = تسک idle (رزرو شده) |
| `period_ms` | `uint32_t` | 0 = غیردوره‌ای (هر زمان زمان‌بندی شود اجرا می‌شود)، >0 = دوره‌ای |
| `stack_bytes` | `uint32_t` | اندازه پشته به بایت (پیش‌فرض: 512) |

### ۲.۲ شروع و توقف تسک‌ها

```cpp
os_task_start(task_function);    // فعال‌سازی تسک متوقف‌شده
os_task_stop(task_function);     // غیرفعال‌سازی (حافظه آزاد نمی‌شود)

// بررسی وضعیت
bool running = os_task_isActive(task_function);
uint8_t state = os_task_get_state(task_function); // enum TaskState
```

> **💡 نکته:** توقف تسک حافظه آن را آزاد نمی‌کند — حافظه به‌صورت ایستا تخصیص یافته است. می‌توانید همان تسک را هر تعداد بار که نیاز دارید شروع/توقف کنید. این مکانیسم اصلی کنترل تسک در زمان اجراست.

### ۲.۳ اولویت تسک

```cpp
uint8_t prio = os_get_task_priority(task_function);
// اولویت فعال را برمی‌گرداند (ممکن است موقتاً توسط سقف IPC افزایش یافته باشد)
```

**راهنمای اولویت‌ها:**
- 1: کمترین اولویت برنامه
- 2–4: تسک‌های پس‌زمینه (حسگرها، ثبت‌گر)
- 5–10: تسک‌های عادی (رابط کاربری، ارتباطات)
- 11–20: تسک‌های با اولویت بالا (پایش ایمنی)
- 21+: تسک‌های شبیه ISR (بسیار حیاتی از نظر زمانی)
- 255: رزرو شده (استفاده نکنید)

### ۲.۴ تسک‌های دوره‌ای

```cpp
// تسک هر 100 میلی‌ثانیه به‌صورت خودکار اجرا می‌شود
void task_sensor(void) {
    while (1) {
        uint16_t adc_val = read_adc();
        // ... پردازش داده ...
        os_delay_ms(10); // واگذاری — اجرای بعدی در مرز دوره بعدی برنامه‌ریزی می‌شود
    }
}

// ایجاد با دوره 100 میلی‌ثانیه
os_task_create(task_sensor, 3, 100);
```

> **💡 نکته:** برای تسک‌های دوره‌ای، از `os_delay_ms()` در انتهای هر تکرار استفاده کنید. زمان‌بند به‌طور خودکار تسک را در مرز دوره بعدی بیدار می‌کند. برای توضیح کامل، بخش [۹](#۹-تسک‌های-دوره‌ای--روش-zenos-بدون-تایمر-نرم‌افزاری) را ببینید.

### ۲.۵ راهنمای اندازه پشته

| نوع تسک | پشته پیشنهادی |
|----------|---------------|
| چشمک‌زن LED ساده | 128–256 بایت |
| خواندن حسگر (HAL) | 256–512 بایت |
| ارتباط UART | 512–1024 بایت |
| SPI/I2C + پشته پروتکل | 512–1024 بایت |
| printf / فرمت‌بندی اعشاری | 1024–2048 بایت |
| منطق برنامه پیچیده | 1024–2048 بایت |

> **💡 نکته:** از `os_get_stack_usage(task)` در زمان توسعه استفاده کنید تا مصرف واقعی اوج را اندازه بگیرید، سپس پشته را ≥130% اوج تنظیم کنید.

---

## ۳. زمان‌بندی و تأخیرها

### ۳.۱ تأخیر میلی‌ثانیه‌ای

```cpp
os_delay_ms(100);  // خواب به مدت ~100ms، واگذاری CPU به تسک‌های دیگر
os_delay_ms(0);    // واگذاری فوری (به تسک‌های دیگر فرصت اجرا می‌دهد)
```

### ۳.۲ تأخیر میکروثانیه‌ای (就业岗位)

```cpp
os_delay_us(10);    // انتظار شلوغ ~10µs (CPU را بلاک می‌کند — هیچ تسک دیگری اجرا نمی‌شود!)
os_delay_us(1000);  // انتظار شلوغ ~1ms
```

> **⚠️ هشدار:** `os_delay_us()` از شمارنده چرخه DWT برای انتظار شلوغ استفاده می‌کند. CPU را واگذار **نمی‌کند**. فقط برای تأخیرهای بسیار کوتاه (< 1ms) استفاده کنید. برای تأخیرهای طولانی‌تر از `os_delay_ms()` استفاده کنید.

### ۳.۳ دریافت زمان جاری

```cpp
uint32_t ticks = os_get_tick();  // شمارنده خام تیک
uint32_t ms    = os_get_ms();    // میلی‌ثانیه از زمان بوت
uint32_t us    = os_get_us();    // میکروثانیه از زمان بوت (شمارنده چرخه DWT)
```

### ۳.۴ الگوهای زمانی

**اندازه‌گیری زمان سپری‌شده:**
```cpp
uint32_t t0 = os_get_ms();
// ... انجام کار ...
uint32_t elapsed = os_get_ms() - t0;  // حتی اگر ms بیش‌تر شود کار می‌کند (~49 روز)
```

**بررسی دوره‌ای بدون بلاک:**
```cpp
static uint32_t last_check = 0;
if (os_get_ms() - last_check >= 100) {
    last_check = os_get_ms();
    // ... انجام کار دوره‌ای ...
}
```

> **💡 نکته:** همیشه از الگوی تفریق `os_get_ms() - last >= interval` به‌جای مقایسه مطلق استفاده کنید. این روش درست شمارنده میلی‌ثانیه 32 بیتی را مدیریت می‌کند.

---

## ۴. بخش‌های حیاتی (OS_SAFE)

### ۴.۱ استفاده پایه

```cpp
volatile uint32_t shared_counter = 0;

// در تسک A:
OS_SAFE {
    shared_counter++;  // در اینجا وقفه‌ها غیرفعال هستند
}
// در اینجا وقفه‌ها دوباره فعال می‌شوند
```

`OS_SAFE` تمام وقفه‌ها را در مدت بلاک غیرفعال و سپس دوباره فعال می‌کند. این **تنها راه امن** برای محافظت از متغیرهای مشترک بین تسک‌ها و ISRهاست.

### ۴.۲ کاری که OS_SAFE انجام می‌دهد

```
┌─ OS_SAFE { ─────────────────────────┐
│  1. غیرفعال‌سازی وقفه‌ها (CPSID I)   │
│  2. ورود به بلاک محافظت‌شده          │
│  3. فعال‌سازی وقفه‌ها (CPSIE I)       │
└──────────────────────────────────────┘
```

پیاده‌سازی از RAII (کلاس C++ `_OsSafeGuard`) استفاده می‌کند — وقفه‌ها همیشه دوباره فعال می‌شوند، حتی اگر استثنا رخ دهد.

### ۴.۳ تو در تو

```cpp
OS_SAFE {
    shared_counter++;
    OS_SAFE {
        // تو در تو — عمق ردیابی می‌شود، وضعیت وقفه حفظ می‌شود
        another_var++;
    }
    // هنوز در OS_SAFE بیرونی — وقفه‌ها هنوز غیرفعال‌اند
}
// در اینجا وقفه‌ها دوباره فعال می‌شوند
```

### ۴.۴ پایش مدت زمان

اگر `OS_SAFETY_MAX_CRITICAL_US` پیکربندی شده باشد (پیش‌فرض 1000µs)، بخش حیاتی که از حد فراتر رود خطای `SAFE_TOO_LONG` ایجاد می‌کند.

**بخش‌های حیاتی را کوتاه نگه دارید!**
```cpp
// ✅ خوب — بخش حیاتی کوتاه
OS_SAFE { shared_counter++; }

// ❌ بد — بخش حیاتی طولانی، تمام وقفه‌ها را بلاک می‌کند
OS_SAFE {
    HAL_UART_Transmit(&huart1, data, len, 1000);  // حدود 10ms بلاک می‌کند!
}
```

### ۴.۵ OS_SAFE در مقابل OS_LOCK

| ویژگی | `OS_SAFE` | `OS_LOCK` |
|--------|----------|----------|
| وقفه‌ها را غیرفعال می‌کند | ✅ بله | ❌ خیر |
| بلاک مجاز است | ❌ خیر (خطا می‌دهد) | ✅ بله |
| از ISRها محافظت می‌کند | ✅ بله | ❌ خیر |
| از تسک‌های دیگر محافظت می‌کند | ✅ بله | ✅ بله |
| مورد استفاده | اشتراک‌گذاری داده با ISRs | محافظت از منابع مشترک بین تسک‌ها |

> **💡 نکته:** فقط زمانی از `OS_SAFE` استفاده کنید که نیاز دارید داده را با توابع وقفه به اشتراک بگذارید. برای همگام‌سازی بین تسک‌ها، ترجیحاً از `OS_LOCK` (قفل) استفاده کنید — وقفه‌ها را بلاک نمی‌کند.

---

## ۵. رویدادها (OS_EVENT)

### ۵.۱ سیگنال‌دهی پایه

```cpp
OS_EVENT sensor_ready;

// تسک A (منتظر):
if (sensor_ready.wait(1000)) {  // حداکثر 1000ms منتظر بمان
    // رویداد سیگنال شد
    process_sensor_data();
} else {
    // تایم‌اوت — رویداد به‌موقع سیگنال نشد
    handle_timeout();
}

// تسک B (سیگنال‌دهنده):
read_sensor();
sensor_ready.signal();  // منتظر را بیدار کن
```

### ۵.۲ سیگنال‌دهی ISR به تسک

```cpp
OS_KEY_PRESS;

// در ISR EXTI:
void EXTI0_IRQHandler(void) {
    OS_KEY_PRESS.signal_from_isr();  // نسخه امن برای ISR
    __HAL_GPIO_EXTI_CLEAR_PIN(...);
}

// در تسک:
void task_handle_input(void) {
    while (1) {
        if (OS_KEY_PRESS.wait(5000)) {
            // کلید فشرده شد
        }
    }
}
```

> **💡 نکته:** همیشه در توابع وقفه از `signal_from_isr()` استفاده کنید. فراخوانی `signal()` از ISR رفتار تعریف‌نشده است.

### ۵.۳ انباشت سیگنال

```cpp
OS_EVENT evt;

evt.signal();  // شمارنده = 1
evt.signal();  // شمارنده = 2
evt.signal();  // شمارنده = 3

evt.wait();    // شمارنده = 2 (مقدار 1 برمی‌گرداند)
evt.wait();    // شمارنده = 1 (مقدار 1 برمی‌گرداند)
evt.wait();    // شمارنده = 0 (مقدار 1 برمی‌گرداند)
evt.wait();    // مقدار 0 برمی‌گرداند (تایم‌اوت — شمارنده قبلاً 0 بود)
```

### ۵.۴ انتظار برای چندین رویداد

```cpp
OS_EVENT evt_a, evt_b;

// انتظار برای هر کدام (هر کدام زودتر بیاید)
uint32_t mask = evt_a | evt_b;  // فقط برای شناسه‌های 0-31 کار می‌کند
// از os_event_signal_from_isr(mask) برای سیگنال‌دهی ISR استفاده کنید

// انتظار فردی با تایم‌اوت کوتاه به‌عنوان polling
while (1) {
    if (evt_a.wait(10) == 1) {
        handle_a();
    }
    if (evt_b.wait(10) == 1) {
        handle_b();
    }
}
```

### ۵.۵ محدودیت‌های شناسه رویداد

- رویدادها شناسه‌های خودکار از 0 شروع می‌شوند
- شناسه‌های 0–31: پشتیبانی کامل (سیگنال‌دهی مبتنی بر ماسک، عملگر `|`)
- شناسه‌های ≥ 32: پشتیبانی می‌شوند اما باید به‌صورت جداگانه سیگنال داده شوند (بدون ماسک)
- محدودیت عملی: صدها رویداد در هر سیستم

---

## ۶. قفل‌ها (OS_MUTEX)

### ۶.۱ استفاده پایه

```cpp
OS_MUTEX uart_mtx;  // قفل سراسری

// تسک A:
OS_LOCK(uart_mtx) {
    HAL_UART_Transmit(&huart1, data_a, len, 100);
}  // به‌طور خودکار در پایان بلاک باز می‌شود

// تسک B:
OS_LOCK(uart_mtx) {
    HAL_UART_Transmit(&huart1, data_b, len, 100);
}
```

`OS_LOCK` یک محافظ قفل RAII است — در ورود قفل را به دست می‌آورد و در خروج (شامل برگشت‌های زودهنگام و استثناها) آن را آزاد می‌کند.

### ۶.۲ قفل/باز کردن دستی

```cpp
OS_MUTEX mtx;

if (mtx.lock(1000)) {  // حداکثر 1000ms منتظر بمان
    // قفل به دست آمد
    do_work();
    mtx.unlock();
} else {
    // تایم‌اوت
}
```

### ۶.۳ قفل بازگشتی

```cpp
OS_MUTEX mtx;

OS_LOCK(mtx) {
    OS_LOCK(mtx) {  // بازگشتی — همان تسک می‌تواند دوباره قفل کند
        OS_LOCK(mtx) {  // تو در تو سه‌گانه هم کار می‌کند
            // هنوز محافظت شده
        }
    }
}  // هر سه سطح باز می‌شوند
```

### ۶.۴ سقف IPC (ارث‌بری اولویت)

سقف IPC از وارونگی اولویت جلوگیری می‌کند — یک مشکل ایمنی حیاتی در سیستم‌های بلادرنگ.

```cpp
// اولویت‌های تسک: sensor=2, controller=5, safety=8

// قفل با سقف 8 (اولویت بالاترین کاربر)
OS_MUTEX shared_data_mtx(8);

// sensor (اولویت 2) قفل را به دست می‌آورد → موقتاً به اولویت 8 افزایش می‌یابد
// safety (اولویت 8) منتظر قفل می‌ماند → sensor افزایش می‌یابد، فوراً اجرا می‌شود
// controller (اولویت 5) نمی‌تواند پیشی بگیرد → وارونگی نیست!
```

**نحوه عملکرد:**
1. وقتی تسکی قفل را به دست می‌آورد، اولویتش به سقف افزایش می‌یابد
2. سقف باید روی اولویت بالاترین تسکی که قفل را به دست می‌آورد تنظیم شود
3. در باز کردن، اولویت به مقدار اصلی (پایه) برمی‌گردد

```cpp
// پیکربندی صحیح سقف
OS_MUTEX sensor_mtx(8);  // سقف = 8 (اولویت تسک safety)
// sensor(2)، controller(5)، safety(8) همه از این قفل استفاده می‌کنند
// وقتی sensor آن را نگه دارد، sensor با اولویت 8 اجرا می‌شود
// safety هرگز منتظر controller نمی‌ماند — وارونگی جلوگیری شد
```

> **💡 نکته:** سقف را روی اولویت **بالاترین تسکی** که هرگز قفل را به دست می‌آورد تنظیم کنید. سقف نادرست می‌تواند باعث وارونگی اولویت یا تضاد اولویت شود.

### ۶.۵ قوانین ایمنی قفل

| قانون | نتیجه نقض |
|--------|-----------|
| هر قفلی را که می‌گیرید باز کنید | قفل همیشه بسته می‌ماند، تسک‌های دیگر deadlock می‌کنند |
| قفل/باز کردن در همان scope | خطر فراموش کردن باز کردن |
| هرگز از ISR قفل نکنید | تا ابد بلاک می‌شود — ISR نمی‌تواند واگذاری کند |
| هرگز `OS_SAFE` + `OS_LOCK` را با هم استفاده نکنید | `OS_SAFE` وقفه‌ها را غیرفعال می‌کند، `OS_LOCK` به آن‌ها نیاز دارد |
| تعداد قفل و باز کردن برابر باشد | باز کردن مضاعف تحمل می‌شود اما شمارنده خطا افزایش می‌یابد |

> **💡 نکته:** همیشه `OS_LOCK(mtx) { ... }` را به‌جای `lock()`/`unlock()` دستی ترجیح دهید. محافظ RAII تضمین می‌کند که قفل همیشه آزاد شود، حتی اگر زود برگردید یا استثنا رخ دهد.

---

## ۷. صفوف (OS_QUEUE)

### ۷.۱ استفاده پایه

```cpp
OS_QUEUE<int, 8> sensor_queue;  // ظرفیت: 8 آیتم از نوع int

// تسک تولیدکننده:
sensor_queue.put(42);          // اگر پر باشد بلاک می‌کند
sensor_queue.put(100, 100);    // حداکثر 100ms منتظر، اگر هنوز پر باشد false برمی‌گرداند

// تسک مصرف‌کننده:
int value;
if (sensor_queue.get(value, 1000)) {  // حداکثر 1000ms منتظر بمان
    process(value);
}
```

### ۷.۲ عملیات صف امن ISR

```cpp
OS_QUEUE<uint16_t, 32> adc_queue;

// در ISR ADC:
void ADC_IRQHandler(void) {
    uint16_t val = HAL_ADC_GetValue(&hadc1);
    adc_queue.put_from_isr(val);  // بدون بلاک، اگر پر باشد false برمی‌گرداند
}

// در تسک:
void task_process_adc(void) {
    while (1) {
        uint16_t val;
        if (adc_queue.get(val, 100)) {
            // پردازش مقدار ADC
        }
    }
}
```

> **💡 نکته:** `put_from_isr()` بدون بلاک است — اگر صف پر باشد فوراً `false` برمی‌گرداند. این روش از هر ISR بدون غیرفعال‌سازی وقفه‌ها امن است.

### ۷.۳ پرس‌وجوی وضعیت صف

```cpp
OS_QUEUE<int, 16> q;

q.put(1); q.put(2); q.put(3);

q.get_count();     // 3
q.get_capacity();  // 16
q.is_full();       // false
q.is_empty();      // false

q.reset();         // پاک کردن همه آیتم‌ها
q.is_empty();      // true
q.get_count();     // 0
```

### ۷.۴ صفوف تایپ‌شده

```cpp
// صف ساختارها
struct SensorData {
    uint16_t temperature;
    uint16_t humidity;
    uint32_t timestamp;
};

OS_QUEUE<SensorData, 16> sensor_data_queue;

// صف اشاره‌گرها (برای داده با اندازه متغیر)
OS_QUEUE<void*, 8> msg_queue;
```

> **💡 نکته:** `OS_QUEUE` یک قالب (template) است — ظرفیت در زمان کامپایل ثابت است. ظرفیت را بر اساس سناریوهای بدترین حالت انفجار انتخاب کنید. خیلی کوچک = تولیدکننده بلاک می‌شود؛ خیلی بزرگ = RAM هدر می‌رود.

---

## ۸. سیگنال‌ها (OS_SEMAPHORE)

### ۸.۱ سیگنال دودویی (قفل منبع)

```cpp
OS_SEMAPHORE spi_sem(1);  // مقدار اولیه = 1 (یک منبع)

// تسک A:
spi_sem.wait();   // به دست آوردن (شمارنده → 0)
SPI_Transmit(data);
spi_sem.signal(); // آزاد کردن (شمارنده → 1)

// تسک B:
spi_sem.wait();   // اگر A نگه داشته باشد منتظر می‌ماند
SPI_Transmit(data2);
spi_sem.signal();
```

### ۸.۲ سیگنال شمارشی (مجموعه منابع)

```cpp
OS_SEMAPHORE pool_sem(3);  // 3 منبع موجود

pool_sem.wait();  // شمارنده = 2
pool_sem.wait();  // شمارنده = 1
pool_sem.wait();  // شمارنده = 0
pool_sem.wait(100); // تایم‌اوت — منبعی موجود نیست

pool_sem.signal();  // شمارنده = 1
pool_sem.signal();  // شمارنده = 2
```

### ۸.۳ سیگنال محدود

```cpp
OS_SEMAPHORE bounded_sem(0, 5);  // اولیه 0، حداکثر 5

bounded_sem.signal();  // شمارنده = 1
bounded_sem.signal();  // شمارنده = 2
bounded_sem.signal();  // شمارنده = 3
bounded_sem.signal();  // شمارنده = 4
bounded_sem.signal();  // شمارنده = 5
bounded_sem.signal();  // false برمی‌گرداند — حد نرسیده!
```

### ۸.۴ سیگنال‌دهی امن ISR

```cpp
OS_SEMAPHORE data_ready(0);

// در ISR:
void TIM2_IRQHandler(void) {
    data_ready.signal_from_isr();  // امن برای ISR
}

// در تسک:
void task_process(void) {
    while (1) {
        if (data_ready.wait(1000)) {
            process_data();
        }
    }
}
```

---

## ۹. تسک‌های دوره‌ای — روش ZenOS (بدون تایمر نرم‌افزاری)

### ۹.۱ چرا تایمر نرم‌افزاری وجود ندارد؟

ZenOS یک API تایمر نرم‌افزاری ارائه **نمی‌دهد**. در عوض، از **تسک‌های دوره‌ای** استفاده می‌کند — رویکردی برتر که مزایای زیر را ارائه می‌دهد:

| ویژگی | تسک دوره‌ای | تایمر نرم‌افزاری |
|--------|------------|-----------------|
| **حافظه** | پشته اختصاصی (مستقل) | پشته callback مشترک |
| **زمینه** | زمینه کامل تسک | زمینه callback (محدود) |
| ** بلاک** | ✅ می‌تواند روی IPC، قفل بلاک کند | ❌ باید سریع برگردد |
| **ایمنی** | ✅ سرریز پشته برای هر تسک ردیابی می‌شود | ❌ پشته مشترک — سرریز سخت‌تر ردیابی می‌شود |
| **اولویت** | ✅ مستقل برای هر تسک | ❌ معمولاً با اولویت تسک تایمر اجرا می‌شود |
| **محافظت MPU** | ✅ هر تسک منطقه MPU خود را دارد | ❌ callbackها یک منطقه را به اشتراک می‌گذارند |
| **ایزوله خطا** | ✅ خرابی یک تسک دیگران را تحت تأثیر قرار نمی‌دهد | ❌ خرابی callback تایمر می‌تواند سیستم را خراب کند |
| **لغو** | ✅ `os_task_stop()` | ❌ باید callback را ردیابی و لغو کرد |

### ۹.۲ ایجاد تسک دوره‌ای

```cpp
// روش 1: ایجاد با دوره (برنامه‌ریزی خودکار)
void task_read_sensor(void) {
    while (1) {
        uint16_t val = read_adc();
        send_to_queue(val);
        os_delay_ms(10);  // واگذاری — اجرای بعدی در مرز دوره بعدی
    }
}

os_task_create(task_read_sensor, 3, 100);  // هر 100 میلی‌ثانیه

// روش 2: دوره‌ای دستی (کنترل بیشتر)
void task_control_loop(void) {
    while (1) {
        uint32_t t0 = os_get_ms();
        
        read_sensors();
        compute_output();
        apply_actuator();
        
        // دوره دقیق 10ms با جبران کشش
        uint32_t elapsed = os_get_ms() - t0;
        if (elapsed < 10) {
            os_delay_ms(10 - elapsed);
        }
    }
}

os_task_create(task_control_loop, 10);  // غیردوره‌ای — خودمان زمان‌بندی را مدیریت می‌کنیم
```

### ۹.۳ مقایسه رویکردها

**❌ رویکرد تایمر نرم‌افزاری (در ZenOS موجود نیست):**
```cpp
// فرضی — این در ZenOS وجود ندارد
void timer_callback(void* arg) {
    uint16_t val = read_adc();  // باید سریع برگردد!
    send_to_queue(val);         // نمی‌تواند روی قفل بلاک کند!
}
timer_create(100, timer_callback);  // یک callback برای همه استفاده‌ها
```

**✅ رویکرد تسک دوره‌ای ZenOS:**
```cpp
// هر تسک پشته، اولویت و ایزوله خطای خود را دارد
void task_read_sensor(void) {
    while (1) {
        OS_LOCK(spi_mtx) {                    // می‌تواند از قفل استفاده کند!
            uint16_t val = SPI_Read();         // می‌تواند آزادانه از HAL استفاده کند
            OS_QUEUE_LOCK(sensor_queue) {      // می‌تواند روی صف بلاک کند
                sensor_queue.put(val);
            }
        }
        os_delay_ms(10);
    }
}
os_task_create(task_read_sensor, 3, 100);
```

### ۹.۴ دقت زمانی

برای اجرای دوره‌ای دقیق، زمان اجرا را جبران کنید:

```cpp
void task_pid_controller(void) {
    while (1) {
        uint32_t t0 = os_get_us();
        
        // === کار کنترل ===
        float error = setpoint - read_encoder();
        integral += error * dt;
        float output = Kp * error + Ki * integral + Kd * (error - prev_error);
        set_actuator(output);
        prev_error = error;
        // ===================
        
        // جبران زمان اجرا
        uint32_t elapsed_us = os_get_us() - t0;
        uint32_t period_us = 1000;  // دوره 1ms
        if (elapsed_us < period_us) {
            os_delay_us(period_us - elapsed_us);  // دقت زیر-تیک
        }
        // اگر elapsed_us >= period_us باشد، دیر شده — فوراً تکرار بعدی اجرا شود
    }
}

os_task_create(task_pid_controller, 20);  // اولویت بالا، غیردوره‌ای
```

### ۹.۵ الگوهای ارتباط بین تسک‌ها

**تولیدکننده-مصرف‌کننده با صف:**
```cpp
OS_QUEUE<SensorData, 16> data_queue;

void task_sensor(void) {
    while (1) {
        SensorData s = read_all_sensors();
        data_queue.put(s);  // اگر صف پر باشد بلاک می‌کند (فشار معکوس)
        os_delay_ms(50);
    }
}

void task_logger(void) {
    while (1) {
        SensorData s;
        if (data_queue.get(s, 1000)) {
            log_to_flash(s);
        }
    }
}
```

**ISR → تسک با سیگنال:**
```cpp
OS_SEMAPHORE data_sem(0);

void EXTI1_IRQHandler(void) {
    data_sem.signal_from_isr();
}

void task_handle_event(void) {
    while (1) {
        if (data_sem.wait(5000)) {
            handle_exti_event();
        } else {
            log_timeout();
        }
    }
}
```

---

## ۱۰. مدیریت خطا و پایش

### ۱۰.۱ گزارش‌دهی خطا

```cpp
// گزارش خطای سفارشی
os_log_error(OSError::SENSOR_TIMEOUT, (uint8_t)ErrorSeverity::WARNING);

// بررسی شمارنده‌های خطا
uint32_t total      = os_get_error_count();
uint32_t unexpected = os_get_unexpected_error_count();
uint32_t expected   = os_get_expected_error_count();

OSError last = os_get_last_error();
```

### ۱۰.۲ سرکوب خطاهای مورد انتظار

در زمان تست، ممکن است عمداً خطا ایجاد کنید. از `OS_ERROR_EXPECTED` استفاده کنید تا آن‌ها را از شمارنده غیرمنتظره حذف کنید:

```cpp
// در کد تست:
OS_ERROR_EXPECTED {
    os_event_signal(-1);  // عمداً نامعتبر — خطای مورد انتظار
}
// os_get_unexpected_error_count() افزایش نمی‌یابد
```

### ۱۰.۳ ثبت خطا

```cpp
#if OS_MONITOR_ERROR_LOG
    // ثبت خطا
    os_log_error(OSError::STACK_OVERFLOW, (uint8_t)ErrorSeverity::CRITICAL);

    // خواندن ورودی‌های ثبت (جدیدترین ابتدا)
    uint32_t count = os_get_error_log_count();
    for (uint32_t i = 0; i < count; i++) {
        OSErrorEntry entry = os_get_error_log_entry(i);
        // entry.timestamp_tick — چه زمانی رخ داد
        // entry.code          — کدام خطا
        // entry.task_id       — کدام تسک
        // entry.severity      — INFO, WARNING, CRITICAL
    }
#endif
```

### ۱۰.۴ پایش مصرف پشته

```cpp
#if OS_MONITOR_ENABLED
    // مصرف پشته هر تسک
    uint32_t bytes = os_get_stack_usage(task_function);

    // گزارش کامل
    uint8_t count = os_get_stack_report_count();
    for (uint8_t i = 0; i < count; i++) {
        os_stack_report_entry_t rep;
        os_get_stack_report(i, &rep);
        // rep.name       — نام تسک
        // rep.size_bytes — اندازه کل پشته
        // rep.peak_bytes — مصرف اوج از آخرین شروع/ریست
    }

    // مصرف CPU
    uint8_t total_cpu = os_get_cpu_usage_total();
    uint8_t task_cpu  = os_get_task_cpu_usage(task_function);
#endif
```

### ۱۰.۵ پایش ددلاین

```cpp
#if OS_MONITOR_DEADLINE
    // تنظیم ددلاین برای تسک (باید بعد از ایجاد فراخوانی شود)
    os_task_set_deadline(task_safety, 50);  // ددلاین 50ms

    // بررسی تأخیرها
    uint32_t misses = os_get_deadline_miss_count(task_safety);
    if (misses > 0) {
        // مدیریت تأخیر ددلاین (ثبت، ریست و غیره)
    }
#endif
```

---

## ۱۱. ویژگی‌های ایمنی

### ۱۱.۱ تایمر سخت‌افزاری

```cpp
#if OS_SAFETY_HW_WATCHDOG
    // در یک تسک اختصاصی:
    void task_wdg_feeder(void) {
        while (1) {
            os_hw_watchdog_feed();  // تغذیه بدون قید و شرط
            os_delay_ms(1000);     // هر 1 ثانیه تغذیه ( timeouts ~6.5s)
        }
    }

    // یا تغذیه مشروط (اگر ناسالم باشد رد کن):
    os_hw_watchdog_check();  // فقط اگر error_count < 10 باشد تغذیه می‌کند
#endif
```

### ۱۱.۲ آزمون RAM (پس‌زمینه)

```cpp
#if OS_SAFETY_RAM_TEST
    // از تسک idle یا تسک پس‌زمینه کم‌اولویت فراخوانی کنید
    void task_background(void) {
        while (1) {
            os_ram_test_step();  // هر فراخوانی یک کلمه را آزمایش می‌کند
            
            if (os_ram_test_complete()) {
                if (os_get_ram_test_error_count() > 0) {
                    // خرابی SRAM شناسایی شد!
                    os_log_error(OSError::RAM_TEST_FAIL, 2);
                }
                // پس از تکمیل، فراخوانی os_ram_test_step() ریست و از نو شروع می‌کند
            }
            
            os_yield();  // به تسک‌های دیگر اجازه اجرا بده
        }
    }
#endif
```

### ۱۱.۳ آزمون CRC (یکپارچگی ROM)

```cpp
#if OS_SAFETY_CRC_CHECK
    // در زمان بوت (قبل از os_start):
    os_crc_init();  // محاسبه CRC مورد انتظار روی فلش

    // در تسک پس‌زمینه:
    void task_crc_check(void) {
        while (1) {
            os_crc_check_step();  // هر فراخوانی 64 کلمه را بررسی می‌کند
            
            if (os_crc_check_complete()) {
                if (os_get_crc_error_count() > 0) {
                    // خرابی ROM شناسایی شد!
                    os_log_error(OSError::HARDFAULT, 2);
                    // در نظر بگیرید یک ریست کنترل‌شده ایجاد کنید
                }
            }
            
            os_delay_ms(100);  // هر 100ms اجرا شود
        }
    }
#endif
```

### ۱۱.۴ محافظت MPU

```cpp
#if OS_SAFETY_MPU
    // راه‌اندازی MPU در زمان بوت (قبل از os_start):
    os_mpu_init();
    os_mpu_enable();

    // اضافه کردن مناطق حافظه اختصاصی هر تسک:
    extern TCB task_sensor_tcb;
    os_mpu_add_region(&task_sensor_tcb,
        0x20001000,   // آدرس پایه
        4096,          // اندازه (4KB)
        3              // AP: دسترسی کامل، غیرقابل اجرا
    );
#endif
```

---

## ۱۲. نکات و ترفندهای پیشرفته

### ۱۲.۱ مدیریت چرخه حیات تسک

```cpp
// ایجاد تمام تسک‌ها از قبل، شروع/توقف در صورت نیاز
os_task_create(task_heater, 5);
os_task_create(task_cooler, 5);
os_task_stop(task_heater);   // در ابتدا غیرفعال
os_task_stop(task_cooler);   // در ابتدا غیرفعال

// کنترل در زمان اجرا
if (temperature > 70) {
    os_task_start(task_cooler);
    os_task_stop(task_heater);
} else if (temperature < 20) {
    os_task_start(task_heater);
    os_task_stop(task_cooler);
}
```

### ۱۲.۲ تخفیف تدریجی

```cpp
void task_main(void) {
    while (1) {
        // تلاش برای حالت دقت بالا
        if (sensor_available()) {
            read_high_accuracy();
        } else {
            // بازگشت به حالت دقت پایین
            read_low_accuracy();
        }
        
        os_delay_ms(100);
    }
}
```

### ۱۲.۳ مدیریت توان با بیکاری بدون تیک

```cpp
// OS_TOOL_TICKLESS_IDLE را در پیکربندی فعال کنید
// سیستم‌عامل از WFI (انتظار برای وقفه) وقتی هیچ تسکی آماده نیست استفاده می‌کند
// این CPU را به‌طور خودکار در حالت کم‌توان قرار می‌دهد

// نکات طراحی برای توان کم:
// 1. به‌جای حلقه‌های就业岗位 از os_delay_ms() استفاده کنید
// 2. تأخیرهای طولانی → خواب عمیق
// 3. تأخیرهای کوتاه → خواب نسبی
// 4. تسک‌های دوره‌ای به‌طور خودکار از WFI بیدار می‌شوند
```

### ۱۲.۴ نکات اشکال‌زدایی

```cpp
// 1. چاپ تعداد تسک در زمان شروع
uart_printf("Tasks: %d\n", os_get_task_count());

// 2. پایش مصرف پشته به‌صورت دوره‌ای
uint32_t peak = os_get_stack_usage(task_function);
uart_printf("Stack peak: %d bytes\n", peak);

// 3. بررسی مصرف CPU
uint8_t cpu = os_get_task_cpu_usage(task_function);
uart_printf("Task CPU: %d%%\n", cpu);

// 4. چاپ نسخه
uart_printf("ZenOS %s\n", os_get_version_string());

// 5. بررسی خطاهای غیرمنتظره
if (os_get_unexpected_error_count() > 0) {
    uart_printf("UNEXPECTED ERRORS: %d\n", os_get_unexpected_error_count());
}
```

### ۱۲.۵ نکات بهینه‌سازی کامپایلر

```cpp
// داده‌های بحرانی عملکرد را اگر از ISRها دسترسی دارند volatile علامت‌گذاری کنید
static volatile uint32_t isr_counter = 0;

// برای به‌روزرسانی کوتاه متغیر مشترک از OS_SAFE استفاده کنید
OS_SAFE { isr_counter++; }

// برای انتقال داده طولانی ISR به تسک، به‌جای آن از صف استفاده کنید
OS_QUEUE<uint16_t, 32> isr_data_queue;
// در ISR: isr_data_queue.put_from_isr(val);
// در تسک: isr_data_queue.get(val, 100);
```

---

## ۱۳. کتابخانه الگوهای رایج

### ۱۳.۱ ماشین حالت در یک تسک

```cpp
enum State { INIT, RUNNING, ERROR, SLEEP };
static State state = INIT;

void task_state_machine(void) {
    while (1) {
        switch (state) {
            case INIT:
                if (hardware_ready()) state = RUNNING;
                break;
            case RUNNING:
                if (error_detected()) state = ERROR;
                else do_work();
                break;
            case ERROR:
                if (error_cleared()) state = RUNNING;
                else enter_safe_state();
                break;
            case SLEEP:
                os_delay_ms(1000);
                break;
        }
        os_delay_ms(10);  // همیشه واگذاری کنید
    }
}
```

### ۱۳.۲ مدیریت دکمه با حذف نویز

```cpp
OS_KEY_EVT;

void EXTI0_IRQHandler(void) {
    OS_KEY_EVT.signal_from_isr();
}

void task_button(void) {
    while (1) {
        if (OS_KEY_EVT.wait(5000)) {
            os_delay_ms(50);  // تأخیر حذف نویز
            if (read_button()) {
                handle_button_press();
            }
        }
    }
}
```

### ۱۳.۳ PWM نرم‌افزاری از طریق تسک دوره‌ای

```cpp
void task_pwm(void) {
    uint8_t duty = 128;  // 50% پرکردگی
    while (1) {
        HAL_GPIO_WritePin(PWM_PORT, PWM_PORT_PIN, GPIO_PIN_SET);
        os_delay_us(duty * 10);  // زمان روشن
        
        HAL_GPIO_WritePin(PWM_PORT, PWM_PORT_PIN, GPIO_PIN_RESET);
        os_delay_us((255 - duty) * 10);  // زمان خاموش
    }
}

os_task_create(task_pwm, 15);  // اولویت بالا برای زمان‌بندی
```

### ۱۳.۴ خط تولید حسگر چندمرحله‌ای

```cpp
OS_QUEUE<RawData, 8> raw_queue;
OS_QUEUE<ProcessedData, 8> processed_queue;

void task_acquire(void) {
    while (1) {
        RawData raw = read_sensor();
        raw_queue.put(raw);
        os_delay_ms(10);
    }
}

void task_process(void) {
    while (1) {
        RawData raw;
        if (raw_queue.get(raw, 100)) {
            ProcessedData proc = filter_and_calibrate(raw);
            processed_queue.put(proc);
        }
    }
}

void task_output(void) {
    while (1) {
        ProcessedData proc;
        if (processed_queue.get(proc, 100)) {
            display(proc);
        }
    }
}

// خط تولید: acquire(3) → process(5) → output(4)
os_task_create(task_acquire, 3, 10);
os_task_create(task_process, 5);
os_task_create(task_output, 4);
```

### ۱۳.۵ خاموش‌سازی نرم

```cpp
static volatile bool shutdown_requested = false;

void task_shutdown_handler(void) {
    while (1) {
        if (shutdown_button.wait(1000)) {
            shutdown_requested = true;
            
            // توقف تسک‌های غیرضروری
            os_task_stop(task_ui);
            os_task_stop(task_led);
            
            // ذخیره وضعیت در فلش
            save_critical_data();
            
            // ورود به حالت ایمن
            enter_safe_mode();
        }
    }
}
```

---

## پیوست: مرجع سریع API

### توابع هسته (C)

| تابع | توضیح |
|------|--------|
| `os_init()` | راه‌اندازی سیستم‌عامل (قبل از ایجاد تسک فراخوانی شود) |
| `os_start()` | شروع زمان‌بند (هرگز برنمی‌گردد) |
| `os_delay_ms(ms)` | خواب به مدت `ms` میلی‌ثانیه |
| `os_delay_us(us)` | انتظار شلوغ به مدت `us` میکروثانیه |
| `os_yield()` | واگذاری به تسک آماده بعدی |
| `os_get_tick()` | دریافت شمارنده خام تیک |
| `os_get_ms()` | دریافت میلی‌ثانیه از زمان بوت |
| `os_get_us()` | دریافت میکروثانیه از زمان بوت |
| `os_task_start(entry)` | شروع تسک متوقف‌شده |
| `os_task_stop(entry)` | توقف تسک در حال اجرا |
| `os_task_isActive(entry)` | بررسی فعال بودن تسک |
| `os_get_task_count()` | دریافت تعداد تسک‌های ثبت‌شده |
| `os_get_task_priority(entry)` | دریافت اولویت فعال تسک |

### کلاس‌های RAII (C++)

| کلاس | ماکرو | توضیح |
|-------|--------|--------|
| `_OsSafeGuard` | `OS_SAFE { ... }` | محافظ غیرفعال‌سازی وقفه |
| `_OsLockGuard` | `OS_LOCK(mtx) { ... }` | محافظ قفل mutex |
| `OS_EVENT` | — | سیگنال‌دهی رویداد بین تسک‌ها |
| `OS_MUTEX` | — | قفل با ارث‌بری اولویت |
| `OS_QUEUE<T,N>` | — | صف FIFO محدود |
| `OS_SEMAPHORE` | — | سیگنال شمارشی |

---

*تولید شده برای ZenOS RTOS v1.0.0 — همچنین ببینید: SAFETY_MANUAL.md، ZenOS_Config.hpp*
