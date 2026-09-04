# Contributing to ZenOS

Thank you for your interest in contributing to ZenOS! This document explains how to get started.

---

## How to Contribute

### 1. Report Bugs

Open an issue with:
- **Description:** What happened vs. what you expected
- **Steps to reproduce:** Minimal code or steps
- **Environment:** MCU family, compiler version, optimization level
- **ZenOS version:** `os_get_version_string()` output

### 2. Suggest Features

Open an issue with:
- **Use case:** Why this feature is needed
- **Proposed API:** How it would be used
- **Safety impact:** Does this affect safety mechanisms?

### 3. Submit Code

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Make your changes
4. Add tests in `Src/main.cpp` if applicable
5. Ensure all tests pass on hardware
6. Commit with a descriptive message
7. Push and open a Pull Request

---

## Code Standards

### Language

- **C++11** (no C++17, no RTTI, no exceptions)
- `static_assert` for compile-time invariants
- RAII for resource management (locks, critical sections)
- `extern "C"` for all public API functions

### Naming

| Element | Convention | Example |
|---------|-----------|---------|
| Functions (C) | `snake_case` with `os_` prefix | `os_delay_ms()` |
| Classes | `UPPER_CASE` | `OS_MUTEX`, `OS_EVENT` |
| Macros | `UPPER_CASE` with `OS_` prefix | `OS_SAFE`, `OS_LOCK` |
| Template params | `PascalCase` | `StackBytes`, `Capacity` |
| Local variables | `snake_case` | `task_count`, `error_code` |

### Safety Rules

1. **No heap allocation** — all memory must be statically allocated
2. **No dynamic task creation** after `os_start()`
3. **ISR-safe variants** must be used in interrupt context (`signal_from_isr`, `put_from_isr`)
4. **Critical sections** must be short (< 1ms recommended)
5. **Stack sizes** must be verified via `os_get_stack_usage()`
6. **All public API** must have Doxygen-style comments

### Testing

- Every new feature must have a corresponding test in `main.cpp`
- Tests report via UART: `[PASS] #N: test_name` or `[FAIL] #N: test_name`
- Use `TEST_ASSERT(condition)` macro for assertions
- Use `OS_ERROR_EXPECTED { ... }` for fault injection tests
- The test suite must pass with 0 unexpected errors

### Commit Messages

```
<type>: <short description>

<optional body — explain WHY, not WHAT>

<optional footer>
```

Types: `feat`, `fix`, `docs`, `test`, `refactor`, `style`, `chore`

---

## Project Structure

```
ZenOS/
├── ZenOS/
│   ├── ZenOS.hpp              # Public API (edit carefully — assembly offsets)
│   ├── ZenOS_Config.hpp       # User configuration
│   ├── ZenOS_Port.hpp         # Hardware detection
│   ├── ZenOS_Internal.hpp     # Internal structures
│   ├── ZenOS.cpp              # Kernel core
│   ├── ZenOS_Scheduler.cpp    # Scheduler
│   ├── ZenOS_Safety.cpp       # Safety features
│   ├── ZenOS_IPC.cpp          # IPC primitives
│   └── ZenOS_Monitor.cpp      # Monitoring
├── Src/
│   └── main.cpp               # Application + test suite
└── Startup/                   # ARM startup assembly
```

### Important: TCB Layout

The `TCB` struct in `ZenOS.hpp` has `static_assert` checks that verify byte offsets match the assembly code. **Do not reorder TCB fields** — this will cause a compile error. If you need to add fields, add them after the assembly-critical section.

---

## Getting Help

- Read the [API Tutorial](API_TUTORIAL.md)
- Read the [Safety Manual](SAFETY_MANUAL.md)
- Open an issue for questions

---

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
