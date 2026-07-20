# nRPC 0.1.0

C++23-сервер nRPC, построенный на libuv. Формат передачи данных и поведение
соответствуют [SPEC.md](SPEC.md).

## Сборка

Проекту требуются CMake 3.20 или новее, компилятор C++23 с `std::expected` и
libuv. mbedTLS является необязательной зависимостью и включает TLS-соединения.
GoogleTest нужен только при `BUILD_TESTING=ON`.

Сборка имеет две конфигурации:

- `Debug`: включены отладочные символы, оптимизация отключена
- `Release`: включена оптимизация `O2`, отладочная информация отключена

Выберите целевую платформу с помощью `-DPLATFORM=windows` или
`-DPLATFORM=linux`. Если параметр не указан, CMake выбирает текущую платформу
хоста.

### Windows

Установите Visual Studio 2022 Build Tools или Visual Studio 2022 с workload
**Desktop development with C++**. Собирайте все библиотеки под ту же
архитектуру, что и сервер; команды ниже используют 64-битные библиотеки.

По умолчанию зависимости для Windows ищутся в `thirdparty`. Как минимум
скопируйте заголовки и библиотеку libuv в такую структуру:

```text
thirdparty/
  include/
    uv.h
    uv/...
  libs/
    libuv.lib           # uv_a.lib, libuv_a.lib или uv.lib тоже подходят
```

Чтобы включить поддержку TLS, добавьте заголовки mbedTLS и три библиотеки:

```text
thirdparty/
  include/
    mbedtls/...
    psa/...
  libs/
    mbedtls.lib
    mbedx509.lib
    mbedcrypto.lib
```

Настройте, соберите и протестируйте проект из Developer PowerShell:

```powershell
cmake -S . -B build-windows -A x64 -DPLATFORM=windows -DBUILD_TESTING=ON
cmake --build build-windows --config Debug --parallel
ctest --test-dir build-windows -C Debug --output-on-failure
```

Исполняемый файл находится в `build-windows\Debug\nrpc_server.exe`. Если
mbedTLS отсутствует в `thirdparty`, CMake выводит статусное сообщение и
собирает транспорт без шифрования. Используйте
`-DNRPC_THIRDPARTY_DIR=C:\path\to\dependencies`, чтобы выбрать другой каталог
зависимостей.

Оптимизированную сборку без тестов можно создать так:

```powershell
cmake -S . -B build-windows-release -A x64 -DPLATFORM=windows -DBUILD_TESTING=OFF
cmake --build build-windows-release --config Release --parallel
.\build-windows-release\Release\nrpc_server.exe 9000
```

API бэкенда NI-DAQmx всегда регистрируется. На Windows он загружает DLL
поставщика во время выполнения, поэтому для компиляции сервера не нужны
заголовки поставщика и import-библиотеки. Для NI-DAQmx требуется `nicaiu.dll`
из установленного runtime NI-DAQmx на целевой машине. Если runtime отсутствует,
клиент получает ошибку загрузки runtime от бэкенда.

### Linux (Ubuntu/Debian)

Установите компилятор и пакеты разработки:

```sh
sudo apt update
sudo apt install build-essential cmake pkg-config libuv1-dev libmbedtls-dev libgtest-dev
```

Настройте, соберите и протестируйте:

```sh
cmake -S . -B build-linux \
  -DPLATFORM=linux \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```

Исполняемый файл находится в `build-linux/nrpc_server`. CMake находит libuv и
mbedTLS через их CMake-пакеты или `pkg-config`. Поддержка последовательных
портов в Linux использует нативный POSIX API `termios` и не требует
дополнительной serial-библиотеки. Диапазон функций NI-DAQmx все равно
регистрируется; вызовы возвращают ошибки загрузки runtime, потому что runtime
поставщика доступен только для Windows.

Оптимизированную сборку без тестов можно создать так:

```sh
cmake -S . -B build-linux-release \
  -DPLATFORM=linux \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build-linux-release --parallel
./build-linux-release/nrpc_server 9000
```

### Параметры сборки

| Параметр | По умолчанию | Назначение |
| --- | --- | --- |
| `PLATFORM` | платформа хоста | Должен быть `windows` или `linux` |
| `CMAKE_BUILD_TYPE` | `Debug` | Только для single-config генераторов; должен быть `Debug` или `Release` |
| `BUILD_TESTING` | `ON` | Собирать набор C++-тестов |
| `NRPC_THIRDPARTY_DIR` | `thirdparty` | Каталог зависимостей Windows, содержащий `include/` и `libs/` |
| `NRPC_PYTHON_CLIENT_SOURCE_DIR` | определяется автоматически | Необязательное дерево Python-клиента для интеграционных тестов |

Visual Studio и другие multi-config генераторы предоставляют только `Debug` и
`Release`. Встроенные API TCP, serial и NI-DAQmx бэкендов всегда
компилируются и регистрируются. TLS компилируется, когда найден mbedTLS; иначе
сервер собирается без поддержки TLS и сообщает об ошибке, если TLS-сертификаты
настроены во время выполнения.

Шестнадцатеричные дампы пакетов по умолчанию отключены. Включите их во время
выполнения через `packet_dump = true` в конфигурации сервера.
Вывод времени выполнения RPC также по умолчанию отключён; он включается через
`rpc_timing = true` и записывается на уровне `DEBUG` в микросекундах.

Чтобы установить только исполняемый файл сервера в staging-каталог:

```sh
cmake --install build-linux-release \
  --prefix artifacts/nrpc-runtime \
  --component runtime
```

Для сборки Visual Studio добавьте `--config Release` к команде установки.

## Запуск

```sh
./build/nrpc_server 9000
```

Сервер дописывает все логи в `nrpc.log` и дублирует их в стандартный поток
ошибок. Чтобы выбрать другой файл:

```sh
./build/nrpc_server 9000 --log-file /var/log/nrpc/server.log
```

### Конфигурация сервера

При запуске сервер автоматически загружает `nrpc-server.ini`, если файл находится
в одной папке с исполняемым файлом. Текущая рабочая папка при этом не важна.
Также можно явно указать другой `.ini`-файл:

```sh
./build/nrpc_server --config nrpc-server.ini
```

Пример `nrpc-server.ini`:

```ini
[server]
host = 0.0.0.0
port = 9000
workers = 4
backlog = 128
max_payload_size = 16777216
arena_size = 1048576
default_timeout_ms = 0

[logging]
file = nrpc.log
packet_dump = false
rpc_timing = false

[tls]
enabled = false
certificate = server.crt
private_key = server.key
```

Секция `[server]` управляет слушателем и выполнением запросов:

| Ключ | По умолчанию | Назначение |
| --- | --- | --- |
| `host` | `0.0.0.0` | IPv4-адрес для привязки |
| `port` | `9000` | TCP-порт для привязки |
| `workers` | по умолчанию libuv | Задает `UV_THREADPOOL_SIZE` для блокирующей работы бэкендов |
| `backlog` | `128` | TCP listen backlog |
| `max_payload_size` | `16777216` | Максимальный payload nRPC-запроса в байтах |
| `arena_size` | `1048576` | Размер response arena на один запрос в байтах |
| `default_timeout_ms` | `0` | Тайм-аут, используемый, когда запрос не содержит тайм-аут; `0` отключает значение по умолчанию |

Секция `[logging]` управляет выводом логов:

| Ключ | По умолчанию | Назначение |
| --- | --- | --- |
| `file` | `nrpc.log` | Путь к файлу логов |
| `packet_dump` | `false` | Записывать полные шестнадцатеричные дампы пакетов `RX Request` и `TX Response` |
| `rpc_timing` | `false` | Записывать время выполнения каждого RPC-обработчика в микросекундах на уровне `DEBUG` |

Секция `[tls]` поддерживает:

| Ключ | По умолчанию | Назначение |
| --- | --- | --- |
| `enabled` | `false` | Включить TLS для слушателя |
| `certificate` | пусто | Путь к сертификату сервера, PEM или DER |
| `private_key` | пусто | Путь к соответствующему приватному ключу, PEM или DER |

TLS требует одновременно `certificate` и `private_key`, когда
`enabled = true`. Логические значения принимают `true/false`, `yes/no`,
`on/off`, `enabled/disabled` и `1/0`.

Параметры командной строки по-прежнему доступны и полезны для переопределений
при запуске как сервис:

```sh
./build/nrpc_server --config nrpc-server.ini --host 127.0.0.1 --workers 8
```

Поддерживаемые параметры: `--config PATH`, `--host HOST`, позиционный `port`,
`--log-file PATH`, `--workers N`, `--tls-cert PATH`, `--tls-key PATH` и
`--no-tls`. Автоматически найденный файл загружается первым, поэтому явно
указанные файлы конфигурации и параметры командной строки переопределяют его
значения в порядке аргументов командной строки.

Чтобы шифровать подключения nRPC-клиентов с помощью mbedTLS, соберите проект с
установленным mbedTLS или разместите его заголовки и библиотеки в `thirdparty`
на Windows, затем передайте сертификат сервера и соответствующий приватный
ключ:

```bash
./build/nrpc_server 9000 --tls-cert server.crt --tls-key server.key
```

TLS включается явно; без этих двух параметров сервер продолжает принимать
обычные TCP-соединения. Слушатель с включенным TLS принимает только
TLS-соединения.

Каждая строка сбрасывается сразу. Когда `packet_dump = true`, полные
шестнадцатеричные дампы пакетов `RX Request` и `TX Response` записываются в тот
же файл.
Когда `rpc_timing = true`, каждый завершённый обработчик записывает строку вида
`RPC Timing client=1 function=16 status=success duration_us=1234`.

Пример исполняемого файла регистрирует две имитационные функции:

- `0`: echo с arena-backed буфером
- `1`: блокирующий sleep, принимающий одну длительность `uint32` в мс в сетевом порядке байтов
- `16`-`19`: кроссплатформенный TCP-бэкенд
- `32`-`35`: serial-бэкенд
- `64`-`74`: Windows-бэкенд NI-DAQmx

Sleep-бэкенд существует для проверки изоляции worker-потоков и обработки
тайм-аутов.

## TCP-бэкенд

TCP-бэкенд использует сокеты операционной системы напрямую:

- POSIX-сокеты на Linux и других Unix-подобных системах
- Winsock 2 на Windows, с линковкой через `ws2_32`
- `getaddrinfo` для IPv4/IPv6 и разрешения имен хостов
- блокирующие вызовы отправки/приема, изолированные в worker-потоках libuv
- распространение тайм-аута запроса на операции connect, send и receive
- автоматическое закрытие сокета при отключении владеющего nRPC-клиента

`register_tcp_backend(dispatcher)` устанавливает такую таблицу функций:

| Индекс | Операция | Аргументы | Результат |
| ---: | --- | --- | --- |
| 16 | Open | `string host`, `uint16 port` | `uint64 handle` |
| 17 | Write | `uint64 handle`, `bytes data` | `uint32 bytes_written` |
| 18 | Read | `uint64 handle`, `uint32 max_size` | `bytes data` |
| 19 | Close | `uint64 handle` | нет переменных |

Handle ограничен nRPC-соединением, которое его создало, и не может
использоваться другим клиентом.

## Serial-бэкенд

Serial-бэкенд использует нативные Win32 serial handles для COM-портов на
Windows и POSIX file descriptors `termios` на Linux и macOS. Блокирующие
операции выполняются в worker pool libuv, а handles автоматически закрываются
при отключении владеющего nRPC-клиента.

`register_serial_backend(dispatcher)` устанавливает такую таблицу функций:

| Индекс | Операция | Аргументы | Результат |
| ---: | --- | --- | --- |
| 32 | Open | `string name`, `uint32 baud`, четыре настройки `uint8` | `uint64 handle` |
| 33 | Write | `uint64 handle`, `bytes data` | `uint32 bytes_written` |
| 34 | Read | `uint64 handle`, `uint32 max_size` | `bytes data` |
| 35 | Close | `uint64 handle` | нет переменных |

Четыре настройки открытия: биты данных, четность, стоп-биты и управление
потоком. Коды четности: none `0`, odd `1`, even `2`, mark `3` и space `4`.
Коды управления потоком: none `0`, software `1`, RTS/CTS `2` и DTR/DSR `3`.
POSIX-бэкенд принимает скорости передачи, доступные через `termios` API хоста.
Платформенно-зависимые режимы, которые нельзя представить через `termios`,
возвращают ошибку `3008`; сюда входят управление потоком DTR/DSR и, на macOS,
четность mark/space.

## NI-DAQmx-бэкенд

Доступный только на Windows NI-DAQmx-бэкенд загружает `nicaiu.dll` во время выполнения,
поэтому сборка сервера не требует заголовков NI или import-библиотек.
Установите runtime NI-DAQmx на машину, к которой подключено оборудование.
Задачи принадлежат клиенту, который их создал, и автоматически останавливаются
и очищаются при закрытии или отключении.

`register_nidaqmx_backend(dispatcher)` устанавливает такую таблицу функций:

| Индекс | Операция | Аргументы | Результат |
| ---: | --- | --- | --- |
| 64 | Create AI voltage | `string channel`, `float64 min/max`, `int32 terminal` | `uint64 handle` |
| 65 | Create AI thermocouple | channel, min/max, type, CJC source/value/channel | `uint64 handle` |
| 66 | Create AO voltage | `string channel`, `float64 min/max` | `uint64 handle` |
| 67 | Create digital input | `string lines`, `bool all-lines grouping` | `uint64 handle` |
| 68 | Create digital output | `string lines`, `bool all-lines grouping` | `uint64 handle` |
| 69 | Start task | `uint64 handle` | нет переменных |
| 70 | Read analog | `uint64 handle`, `uint32 sample count` | `float64[] values` |
| 71 | Write analog | `uint64 handle`, `float64[] values` | `uint32 samples written` |
| 72 | Read digital | `uint64 handle`, `uint32 sample count` | `uint32[] values` |
| 73 | Write digital | `uint64 handle`, `uint32[] values` | `uint32 samples written` |
| 74 | Close task | `uint64 handle` | нет переменных |

Значения terminal configuration: default `-1`, RSE `10083`, NRSE `10078`,
differential `10106` и pseudodifferential `12529`. Типы thermocouple используют
значения NI-DAQmx: B `10047`, E `10055`, J `10072`, K `10073`, N `10077`,
R `10082`, S `10085` и T `10086`. CJC sources: built-in `10200`, constant
`10116` и channel `10113`. Тайм-аут запроса передается во все операции чтения
и записи. Это подмножество создает on-demand single-channel tasks; hardware-timed
sampling и явная настройка clock находятся вне его области.

## API сервера

Зарегистрируйте бэкенд через `nrpc::Dispatcher::register_handler`. Handler
получает client context, декодированные variables и response arena фиксированной
емкости. Он возвращает `nrpc::BackendResult`, то есть `std::expected`,
содержащий views переменных ответа или `BackendError`.

Данные ответа должны находиться в переданной arena до возврата из handler:

```cpp
dispatcher.register_handler(
    10, [](const nrpc::RequestContext& context,
           std::span<const nrpc::protocol::Variable> arguments,
           nrpc::Arena& arena) -> nrpc::BackendResult {
        auto output = arena.allocate(4, 4);
        if (!output) {
            return std::unexpected(nrpc::BackendError{
                nrpc::error_code::arena_exhausted, "arena exhausted"});
        }
        // Fill output using network byte order.
        return std::vector<nrpc::protocol::VariableView>{{
            nrpc::protocol::DataType::uint32, 1, std::as_const(*output)}};
    });
```

`ResourceManager` владеет некопируемыми объектами `NrpcHandle` и закрывает все
ресурсы, связанные с клиентом, когда его TCP-соединение завершается.

## Python-клиент

Синхронная Python-библиотека поддерживается в отдельном репозитории
`nrpc-python-client`. Держите оба репозитория рядом друг с другом, чтобы
автоматически включить cross-repository integration tests, или настройте ее
расположение явно:

```sh
cmake -S . -B build \
  -DNRPC_PYTHON_CLIENT_SOURCE_DIR=/path/to/nrpc-python-client
```

Без этого пути сервер и все C++ unit tests собираются независимо.

## Ошибки

Коды ошибок, зарезервированные сервером:

| Код | Значение |
| ---: | --- |
| 1001 | Некорректный protocol frame |
| 1002 | Тайм-аут операции сервера |
| 1003 | Response arena исчерпана |
| 1004 | Внутренняя ошибка сервера |
| 2001 | Некорректные аргументы TCP-бэкенда |
| 2002 | Не удалось разрешить имя хоста TCP |
| 2003 | Не удалось установить TCP-соединение |
| 2004 | Некорректный или чужой TCP handle |
| 2005 | Ошибка записи TCP |
| 2006 | Ошибка чтения TCP или peer отключился |
| 2007 | Истек тайм-аут операции TCP-сокета |
| 3001 | Некорректные аргументы serial-бэкенда |
| 3002 | Не удалось открыть serial port |
| 3003 | Ошибка настройки serial |
| 3004 | Некорректный или чужой serial handle |
| 3005 | Ошибка записи serial |
| 3006 | Ошибка чтения serial |
| 3007 | Истек тайм-аут операции serial |
| 3008 | Операция serial не поддерживается |
| 5001 | Некорректные аргументы NI-DAQmx-бэкенда |
| 5002 | Не удалось загрузить runtime NI-DAQmx |
| 5003 | Не удалось создать задачу/канал NI-DAQmx |
| 5004 | Некорректный или чужой handle задачи NI-DAQmx |
| 5005 | Ошибка операции NI-DAQmx |
| 5006 | Операция не соответствует типу канала задачи |

Остальные коды ошибок бэкендов передаются без изменений. Python-клиент
выбрасывает `RemoteError` для ошибок сервера/бэкенда и `ProtocolError` для
некорректно сформированных ответов.

## Архитектура

Цикл libuv владеет accept, read, framing, timers и writes. Полные запросы
передаются через `uv_queue_work`, чтобы блокирующие драйверы оборудования не
попадали в I/O thread. Completion callbacks сериализуют arena-backed results и
возвращают их в loop thread. Соединения используют reference counting, поэтому
отключения и тайм-ауты не могут инвалидировать незавершенные worker jobs.

Каждое соединение допускает один выполняющийся запрос. Чтение происходит в две
стадии framing: фиксированный 16-байтный заголовок, затем ровно `Payload Size`
байт.

## End-to-End-тесты

`tests/mock_scpi_power_supply.py` - автономный симулятор TCP-инструмента с
состоянием и командами `VOLT`, `CURR`, `OUTP`, `MEAS:*`, `SYST:ERR?`, `*IDN?`
и `*RST`. End-to-end test запускает этот симулятор и реально скомпилированный
`nrpc_server`, затем маршрутизирует SCPI-трафик через функции TCP-бэкенда
16-19.

Cross-repository tests требуют Python 3.10 или новее и отдельный репозиторий
клиента. Используйте подробный вывод CTest и `packet_dump = true` в
конфигурации сервера, чтобы видеть логи слушателя и полные дампы пакетов nRPC:

```sh
cmake -S . -B build -DPLATFORM=linux -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build -V -R integration.tcp_backend_scpi
```
