# LComp 7.0.0.4: нативный pipeline АЦП/ЦАП

## Границы и источники истины

Нормативные выводы в этом документе основаны только на материалах вендора:

1. `manual.pdf` из дистрибутива LComp 7.0.0.4, 121 страница;
2. интерфейсы и структуры из `include\ifc_ldev.h`, `include\ioctl.h`, `include\create.h`;
3. реализация пользовательской DLL из `DLL\ldevbase.cpp`, `DLL\ldevbase_ex.cpp` и классов плат `DLL\plx.cpp`, `DLL\791.cpp`, `DLL\e140.cpp`, `DLL\e154.cpp`, `DLL\e2010.cpp`, `DLL\e440.cpp`;
4. штатные примеры `l7xx.tst`, `L7XX.OSC`, `l7xx2.osc`, `l791.gnr`, `WLCOMP_TST2`.

Все эти файлы находятся в `C:\Program Files (x86)\LCard\LIBRARY`. Переданный пользователем PDF побайтно совпадает с `LIBRARY\manual.pdf` и `LIBRARY\help\manual.pdf`.

Репозиторий `RAA80/python-lcomp` не используется как источник сведений о железе, форматах данных или семантике параметров. Это только пример вызова экспортов vendor-адаптера `wlcomp.dll` из Python. Замечания о нем вынесены в последний раздел.

## Какой API считается основным

Основной API вендора - C++-интерфейс `IDaqLDevice`:

```cpp
LUnknown* pUnknown = CreateInstance(slot);
IDaqLDevice* dev = nullptr;
pUnknown->QueryInterface(IID_ILDEV, reinterpret_cast<void**>(&dev));
pUnknown->Release();
```

Для независимого запуска ADC и DAC используется `IDaqLDevice2`:

```cpp
IDaqLDevice2* dev2 = nullptr;
dev->QueryInterface(IID_ILDEV2, reinterpret_cast<void**>(&dev2));
```

Нативные структуры:

- `ASYNC_PAR` - одиночные операции через `IoAsync`;
- `ADC_PAR_0` - L-761/780/783, E14-440, E14-140, E-154;
- `ADC_PAR_1` - L-791 и E20-10;
- `DAC_PAR_0` - L-761/780/783, E14-440, E14-140M;
- `DAC_PAR_1` - L-791;
- объединения `ADC_PAR` и `DAC_PAR` позволяют выбирать `t1`/`t2`.

Типы `WADC_PAR_*` и `WDAC_PAR_*` - упакованное C-представление для `wlcomp.dll` и Linux C ABI. Они не являются отдельным аппаратным API и в основном тексте не используются.

## Общий жизненный цикл устройства

```cpp
ULONG status = dev->OpenLDevice();
SLOT_PAR slot = {};
dev->GetSlotParam(&slot);

// Для плат с загружаемым firmware:
dev->LoadBios(biosFile);
dev->PlataTest();

PLATA_DESCR_U2 descr = {};
dev->ReadPlataDescr(&descr);

// ... работа ...

dev->CloseLDevice();
dev->Release();
```

Практические правила:

- каждый вызов проверяется на `L_SUCCESS`; `L_NOTSUPPORTED` нельзя смешивать с общей ошибкой;
- `ReadPlataDescr()` выполняется до расчета частот и калибровки;
- перед использованием DAC проверяется соответствующее поле `IsDacPresent`;
- любой запущенный stream останавливается в cleanup-секции;
- для E20-10 выбирается правильный PLD: `e2010` для исходной ревизии, `e2010m` для E20-10B;
- L-791 не имеет сигнального процессора и BIOS не загружает.

## Матрица возможностей LComp 7.0.0.4

| Устройство | Одиночный ADC | ADC stream/кадр | Одиночный DAC | DAC stream |
|---|---:|---:|---:|---:|
| L-761/L-780/L-783 | `IoAsync` | `ADC_PAR_0` | `IoAsync`, 12-битный код | `DAC_PAR_0`; ограничения ring buffer зависят от ревизии |
| L-791 | `IoAsync` | `ADC_PAR_1` | `IoAsync`, два DAC в одном запросе | `DAC_PAR_1`, 32-битные точки и маркеры прерываний |
| E14-440 | `IoAsync` | `ADC_PAR_0` | `IoAsync`, 12-битный код | `DAC_PAR_0`, 16-битные слова |
| E14-140 | `IoAsync` | `ADC_PAR_0` | `IoAsync`; особый `Mode=2` у rev B | только модификация M/rev не A |
| E-154 | `IoAsync` | `ADC_PAR_0` | `IoAsync`, DLL использует младшие 8 бит | не реализован в данном классе LComp |
| E20-10/E20-10B | не заявлен и не реализован | `ADC_PAR_1` | `IoAsync`, 12-битный код | не реализован в данном классе LComp |

## 1. Получить один отсчет ADC

### Поддерживаемые устройства

Для L-761/780/783, L-791, E14-440, E14-140 и E-154 применяется `IoAsync`:

```cpp
ASYNC_PAR p = {};
p.s_Type = L_ASYNC_ADC_INP;
p.Chn[0] = logicalChannel;

ULONG status = dev->IoAsync(&p);
if (status != L_SUCCESS) {
    // обработать L_NOTSUPPORTED/L_ERROR
}

USHORT raw = static_cast<USHORT>(p.Data[0]);
```

`IoAsync` возвращает один код в `Data[0]`, а не кадр. `Chn[0]` является логическим номером: кроме физического входа в нем закодированы схема подключения и/или диапазон.

### Логический канал

**L-761/L-780**

- bits 0..5 - дифференциальный вход, калибровка нуля или вход с общей землей;
- bits 6..7 - усиление 1/4/16/64.

**L-783**

- раскладка номера аналогична L-761/780;
- bits 6..7 задают усиление 1/2/4/8.

**E14-440/E14-140**

- bits 0..5 задают вход и режим подключения;
- `0x00..0x0F` - 16 дифференциальных входов;
- `0x10..0x1F` - калибровка нуля соответствующего дифференциального входа;
- `0x20..0x3F` - 32 входа с общей землей;
- bits 6..7 задают диапазон ±10, ±2.5, ±0.625 или ±0.15625 В.

**E-154**

- физические каналы 0..7;
- bits 6..7 задают диапазон ±5, ±1.6, ±0.5 или ±0.16 В.

**L-791**

- каналы 0..15;
- bits 6..8 задают один из восьми диапазонов от ±10 до ±0.078 В.

### E20-10

У `DaqE2010` отсутствует реализация `InputADC`, а раздел `IoAsync` мануала не перечисляет одиночный ADC для E20-10. Поэтому корректный vendor-путь - ADC stream с `NCh=1`, после появления первой точки stream останавливается.

### Код ADC и физические единицы

`IoAsync` возвращает аппаратный код. Универсального преобразования «`short` → вольты» для всех плат нет: разрядность, диапазоны и калибровка различаются.

- для плат, где реализован `EnableCorrection`, можно поручить коррекцию LComp/DSP и затем применить масштаб выбранного диапазона;
- для E14-140/E-154/E20-10 мануал отдельно предупреждает, что коррекция возлагается на пользовательское приложение;
- для L-791 коэффициенты читаются через `ReadPlataDescr`; мануал задает коррекцию `Y = (X + B) * A`, причем пары коэффициентов зависят от диапазона ADC;
- нельзя одновременно корректировать код в DSP/LComp и повторно применять те же коэффициенты в приложении.

Точные номинальные формулы перевода кода в напряжение следует брать из руководства конкретной платы. LComp manual описывает API и коэффициенты, но не заменяет метрологический паспорт каждой модели.

## 2. Получить один кадр ADC

Кадр - один последовательный опрос списка `Chn[0..NCh-1]`. В буфере идут кадры один за другим:

```text
Chn[0], Chn[1], ..., Chn[NCh-1], Chn[0], Chn[1], ...
```

Получить кадр одним `IoAsync` нельзя. Используется ADC stream.

### Настройка stream

Последовательность `RequestBufferStream` и `FillDAQparameters` между собой несущественна, но оба вызова должны завершиться до `SetParametersStream`:

```cpp
ULONG requestedWords = 131072;              // размер задается в 16-битных словах
ULONG status = dev->RequestBufferStream(&requestedWords, L_STREAM_ADC);

ADC_PAR adc = {};
// Заполнить adc.t1 или adc.t2 в зависимости от платы.
status = dev->FillDAQparameters(&adc.t1);    // либо &adc.t2

ULONG usedSamples = requestedWords;
void* data = nullptr;
void* sync = nullptr;
status = dev->SetParametersStream(
    &adc.t1,                                // либо &adc.t2
    &usedSamples,
    &data,
    &sync,
    L_STREAM_ADC
);
```

`FillDAQparameters` округляет частоты и нормализует ограничения платы. После него надо использовать фактические `dRate`, `dKadr`, `FIFO`, `IrqStep` и `Pages`, а не исходно запрошенные.

`SetParametersStream` выполняет `DIOC_SETUP`, возвращает адрес mapped buffer и адрес счетчика `sync`. В текущей DLL `UsedSize` вычисляется как `Pages * IrqStep`.

### Запуск только ADC

```cpp
dev2->InitStartLDeviceEx(L_STREAM_ADC);
dev2->StartLDeviceEx(L_STREAM_ADC);

volatile ULONG* syncCounter = static_cast<volatile ULONG*>(sync);

// Ожидать с timeout, пока готово не меньше NCh точек.
// Затем скопировать первый полный кадр из data.

dev2->StopLDeviceEx(L_STREAM_ADC);
```

Наследованные `InitStartLDevice/StartLDevice/StopLDevice` внутри вызывают операции и для ADC, и для DAC. Для независимого управления предпочтителен `IDaqLDevice2`.

Mapped buffer принадлежит драйверу. Данные копируются до освобождения stream; бесконечный busy-wait без timeout применять нельзя.

### Структуры и ограничения плат

**L-761/L-780/L-783: `ADC_PAR_0`**

- `dRate` - частота опроса каналов внутри кадра, кГц;
- `dKadr` - интервал между кадрами, мс;
- `SynchroType`: 0 - цифровой старт, 1 - покадровая синхронизация, 2 - аналоговый старт, 3 - без синхронизации;
- `AutoInit=0` - однократное заполнение большого буфера; `AutoInit=1` - циклическое заполнение;
- форматы усиления L-783 отличаются от L-761/780.

**E14-440/E14-140/E-154: `ADC_PAR_0`**

- `SynchroType`: 0 - без синхронизации, 1 - цифровой старт, 2 - покадровая, 3 - аналоговый старт;
- значения `SynchroType` не совпадают с L-7xx;
- FIFO у E14-140/E-154 не настраивается;
- для E14-140/E-154 `IrqStep` ограничен 64 кОтсчетами;
- `AutoInit` управляет однократным или циклическим заполнением ADC buffer.

**L-791: `ADC_PAR_1`**

- FIFO принимает 1/2/4/8/16/32/64/128 точек;
- `IrqStep <= 2048`;
- длина ring buffer задается `Pages * IrqStep`;
- `AutoInit=0/1` выбирает однократный/циклический ADC;
- `GetParameter(L_POINT_SIZE)` возвращает 4, поэтому mapped ADC data нельзя без проверки трактовать как общий для всех плат `USHORT[]`.

**E20-10/E20-10B: `ADC_PAR_1`**

- FIFO не используется;
- `IrqStep <= 1M` точек;
- `SynchroType` задает источник старта, `SynchroSrc` - тактовый источник;
- `AdcIMask` выбирает signal/GND и диапазон отдельно для четырех аналоговых каналов;
- E20-10B добавляет `DigRate`, `DM_Ena`, `StartCnt`, `StopCnt` и расширенный `SynchroMode`;
- `AutoInit=0/1` относится к однократному/циклическому ADC buffer.

## 3. Вывести один код на DAC

Перед операцией проверяется `IsDacPresent`.

### L-761/L-780/L-783, E14-440, E20-10

```cpp
ASYNC_PAR p = {};
p.s_Type = L_ASYNC_DAC_OUT;
p.Mode = dacChannel;       // 0 или 1
p.Data[0] = rawCode;

ULONG status = dev->IoAsync(&p);
```

Классы этих плат используют младшие 12 бит кода. Для E20-10 перестановку байтов выполняет сама vendor DLL.

### E14-140

Обычный вызов использует 12-битный код и `Mode=0/1`.

Для ревизии B мануал описывает одновременную запись двух полных 16-битных значений:

```cpp
ASYNC_PAR p = {};
p.s_Type = L_ASYNC_DAC_OUT;
p.Mode = 2;
p.Data[0] = codeDac0;
p.Data[1] = codeDac1;
dev->IoAsync(&p);
```

Исходник `DaqE140::OutputDAC` разрешает `Mode=2` только для ревизии не `A`.

### E-154

`DaqE154::OutputDAC` маскирует значение `0xFF`, то есть использует младшие 8 бит. `Mode` канал не выбирает.

### L-791

Оба канала описываются одним запросом; `Chn[i]` является флагом обновления:

```cpp
ASYNC_PAR p = {};
p.s_Type = L_ASYNC_DAC_OUT;
p.Chn[0] = 1;
p.Data[0] = code0;
p.Chn[1] = 0;
p.Data[1] = code1;
dev->IoAsync(&p);
```

`DaqL791::OutputDAC` формирует слово:

```text
bits  0..11  code DAC0
bit      12  enable DAC0
bits 16..27  code DAC1
bit      28  enable DAC1
bit      30  command/strobe
```

Напряжение преобразуется в код по паспорту конкретного DAC с учетом `KoefDAC` из `ReadPlataDescr`. API принимает код, а не значение в вольтах.

## 4. Сформировать произвольный DAC-сигнал

### Общий pipeline vendor API

```cpp
ULONG requestedWords = 512000;
dev->RequestBufferStream(&requestedWords, L_STREAM_DAC);

DAC_PAR dac = {};
// заполнить dac.t1 либо dac.t2
dev->FillDAQparameters(&dac.t1);

ULONG usedSamples = requestedWords;
void* data = nullptr;
void* sync = nullptr;
dev->SetParametersStream(
    &dac.t1,
    &usedSamples,
    &data,
    &sync,
    L_STREAM_DAC
);

// Заполнить data ДО StartLDeviceEx.

dev2->InitStartLDeviceEx(L_STREAM_DAC);
dev2->StartLDeviceEx(L_STREAM_DAC);

// Следить за sync, подкачивать освобожденные части кольца,
// контролировать underrun.

dev2->StopLDeviceEx(L_STREAM_DAC);
```

Минимально значимые поля `DAC_PAR_*`:

- `s_Type = L_DAC_PARAM`;
- `dRate` - частота DAC в кГц; после `FillDAQparameters` содержит достижимое значение;
- `DacEna = 1`;
- `FIFO`, `IrqStep`, `Pages`, `IrqEna` - организация передачи;
- `DacNumber` у большинства streaming-реализаций не выбирает канал: номер находится в самих данных.

### Что делает `AutoInit` у DAC

Для DAC нельзя считать `AutoInit=1` переключателем циклической генерации.

Путь поля в пользовательской библиотеке:

```text
DAC_PAR.AutoInit
  -> FillDAQparameters()
  -> Daq<Board>::FillDACparameters()
  -> внутренняя dac_par
  -> SetParametersStream()
  -> DIOC_SETUP_DAC
```

После этого запуск выполняется отдельными `DIOC_INIT_SYNC` и `DIOC_START`. В пользовательской DLL нет выбора другого ioctl или ветки запуска по `AutoInit`.

- определения `DAC_PAR_0` и `DAC_PAR_1` в мануале помечают поле как «пока не используется»;
- для L-791 мануал отдельно говорит, что `AutoInit` не используется;
- `DaqE140::FillDACparameters` принудительно записывает внутреннее `AutoInit=0`, хотя E14-140M поддерживает streaming;
- L-7xx, E14-440 и L-791 копируют поле дальше, но их пользовательская DLL по нему не ветвится.

Следовательно, непрерывность DAC задается stream-механизмом, кольцевым буфером, `Pages`, `IrqStep`, `FIFO`, прерываниями и явными `Start/Stop`, а не `AutoInit`. `AutoInit=0` не гарантирует однократный проход DAC buffer. Конечный пакет останавливается приложением через `StopLDeviceEx(L_STREAM_DAC)` по счетчику `sync`; точность такой программной остановки зависит от буферизации и планировщика.

### Формат DAC buffer

**L-761/L-780/L-783, вывод из RAM DSP**

- `DAC_PAR_0`;
- данные `USHORT`;
- `IrqStep == FIFO`;
- для плат без настоящего PC ring buffer `Pages` всегда задается 2;
- канал кодируется в bit 12, код - в bits 0..11.

**L-780M rev C, поток из RAM PC**

- мануал отдельно требует данные `ULONG` для PC streaming;
- старый режим из RAM DSP использует `USHORT`;
- это различие сохранено ради совместимости старых драйверов;
- настоящий DAC ring buffer/interrupt mode относится к L-780C.

**E14-440**

- `DAC_PAR_0`, данные `USHORT`;
- канал находится в bit 12;
- `IrqStep == FIFO` и кратен 32;
- исходник DLL ограничивает FIFO диапазоном 32..2016 и `Pages >= 2`.

**E14-140M**

- streaming запрещен для rev A и доступен модификации M/rev не A;
- данные - 16-битные значения;
- каналы выдаются строго попеременно: DAC0, DAC1, DAC0, DAC1;
- `IrqStep` следует задавать 2048, FIFO аппаратно не настраивается;
- фактическая `dRate` выбирается ограниченным делителем и читается после `FillDAQparameters`.

**L-791**

- `DAC_PAR_1`;
- одна точка - `ULONG`, содержащий данные обоих DAC;
- code0 занимает bits 0..11, enable0 - bit 12;
- code1 занимает bits 16..27, enable1 - bit 28;
- каждое `IrqStep`-е слово должно иметь bit 31 = 1 - это маркер генерации прерывания для подкачки;
- `Pages * IrqStep` задает длину кольца;
- FIFO и `AutoInit` не используются;
- `DaqL791::FillDACparameters` ограничивает `dRate` максимумом 125 кГц и квантует ее делителем.

Пример формирования точки L-791:

```cpp
ULONG word =
    (code0 & 0x0FFF) |
    (enable0 ? (1UL << 12) : 0) |
    ((code1 & 0x0FFF) << 16) |
    (enable1 ? (1UL << 28) : 0);

if ((index + 1) % dac.t2.IrqStep == 0) {
    word |= 1UL << 31;
}
```

**E-154 и E20-10**

В соответствующих классах LComp есть одиночный `OutputDAC`, но нет реализации `FillDACparameters`. Поэтому `L_STREAM_DAC` для произвольной формы этим API не поддержан. Цикл из `IoAsync` не эквивалентен hardware stream и не дает детерминированного периода под Windows.

## Работа с кольцевым буфером

`RequestBufferStream` принимает размер в 16-битных словах и округляет его к страницам по 2048 слов. `SetParametersStream` возвращает:

- `Data` - начало области данных после служебной страницы;
- `Sync` - mapped счетчик позиции/заполнения;
- `UsedSize = Pages * IrqStep`.

При непрерывной работе приложение должно хранить собственную consumer/producer position, обрабатывать wrap и не переписывать участок DAC buffer, который еще читает драйвер. Проверка только вида `sync >= buffer_size` подходит для ожидания однократного заполнения, но недостаточна для обслуживания бесконечного кольца.

Разрядность mapped data определяется платой и режимом. Ее следует получать из документации и, где применимо, через `GetParameter(L_POINT_SIZE)`, а не безусловно приводить `void*` к `USHORT*`.

## Роль `wlcomp.dll` и `python-lcomp`

`wlcomp.dll` - поставляемый вендором C-адаптер поверх `IDaqLDevice`. Он:

1. принимает упакованную `WDAQ_PAR`;
2. по `sp_type` создает нативную `ADC_PAR_0/1` или `DAC_PAR_0/1`;
3. вызывает тот же `IDaqLDevice::FillDAQparameters`/`SetParametersStream`;
4. возвращает вызывающему коду скорректированные поля.

`python-lcomp` вызывает именно эти экспорты через `ctypes`. Поэтому его пример полезен для понимания FFI и порядка вызовов, но:

- названия Python-классов и helper-функции не являются частью vendor API;
- преобразование raw ADC в вольты в репозитории является логикой обертки, а не реализацией LComp DLL;
- ограничения плат, форматы DAC buffer и семантика `AutoInit` определяются только vendor manual/source;
- при переносе pipeline в Python надо буквально отображать нативные структуры и сохранять правильную разрядность `Data` для конкретной платы.

