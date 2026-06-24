# AtomSpectra -> BecqMoni REST API

Спецификация: [atomspectra-becqmoni-compat.openapi.yaml](/C:/Users/moroz/source/repos/atomspectra-esp32/openapi/atomspectra-becqmoni-compat.openapi.yaml)

## Что изучено

- Текущая HTTP-реализация в [main/web_server.c](/C:/Users/moroz/source/repos/atomspectra-esp32/main/web_server.c)
- Парсинг спектра и `-inf` в [main/spectrum.c](/C:/Users/moroz/source/repos/atomspectra-esp32/main/spectrum.c)
- Текстовые команды в [main/usb_host_cdc.c](/C:/Users/moroz/source/repos/atomspectra-esp32/main/usb_host_cdc.c)
- Активное поведение .NET-клиента в
  [AtomSpectraDeviceController.cs](/C:/Users/moroz/source/repos/BQ%20Eng%20res%20.NET%204.8/BecquerelMonitor/AtomSpectraDeviceController.cs)
  и
  [AtomSpectraVCPDeviceForm.cs](/C:/Users/moroz/source/repos/BQ%20Eng%20res%20.NET%204.8/BecquerelMonitor/AtomSpectraVCPDeviceForm.cs)

## Ключевой вывод

Текущий REST API уже закрывает выдачу живого спектра, статуса прибора, температур, калибровки и сброс спектра.
Главный пробел для интеграции с BecqMoni: `POST /api/command` не возвращает текстовый ответ прибора и не умеет ждать `-ok` / `-inf` / `-cal`.
Для BecqMoni это не optional-улучшение, а обязательная часть API, потому что активный .NET-код прямо опирается на поведение `sendCommand()`, `waitForAnswer()` и `getCommandOutput()`.

После проверки Android-кода из `C:\Users\moroz\source\repos\atomspectra` выяснилось, что для полного покрытия нужны ещё несколько device-side сценариев:

- `-stt` для определения, идёт ли набор сейчас
- `-mode 0` при подключении USB-прибора
- `-nos N` для изменения noise discriminator
- `-cal` не только для чтения, но и для записи calibration registers обратно в прибор

Именно поэтому в OpenAPI есть два слоя:

- `implemented`: то, что уже есть в прошивке
- `proposed`: новый слой `"/api/v1/..."` для .NET-клиента

## Соответствие активным методам

| .NET источник | Что делает | Предлагаемый REST |
|---|---|---|
| `AtomSpectraVCPDeviceForm.TestConnection` | Проверка доступности прибора и чтение SN | `GET /api/v1/device/test` |
| `AtomSpectraVCPDeviceForm.deadTimeBtn_Click` | Расчёт dead time из `rise`, `fall`, `F` | `GET /api/v1/device/dead-time` |
| `AtomSpectraVCPDeviceForm.CommandLineIn_KeyDown` | Ручная отправка текстовой команды и чтение ответа | `POST /api/v1/commands/execute` |
| `AtomSpectraDeviceController.getCPS()` | Текущий CPS | `GET /api/v1/device/cps` |
| `AtomSpectraDeviceController.getTemp()` | T1 из `-inf` | `GET /api/v1/device/temperature` |
| `AtomSpectraDeviceController.ClearMeasurementResult()` | `-rst` и очистка текущего измерения | `POST /api/v1/measurement/reset` |
| `AtomSpectraDeviceController.StartMeasurement()` | `-sto`, `-rst`, `-sta` | `POST /api/v1/measurement/start` |
| `AtomSpectraDeviceController.AttachToDevice()` | Подцепиться к уже идущему набору | `POST /api/v1/measurement/attach` |
| `AtomSpectraDeviceController.StopMeasurement()` | `-sto` | `POST /api/v1/measurement/stop` |
| `AtomSpectraVCPIn.DataReady` + `update_hystogram()` | Поток спектра, elapsed time, invalid pulses | `GET /api/v1/measurement/live` |

## Что можно переиспользовать без изменений в прошивке

- `GET /api/device`
- `GET /api/spectrum.json`
- `POST /api/reset`
- `GET /api/csrf-token`

## Что надо добавить в прошивку для полной совместимости

- Буфер последних текстовых ответов прибора по `CMD_TEXT`
- Синхронный command-response endpoint для HTTP, который возвращает текстовый ответ прибора и умеет ждать `-ok` / `-inf` / `-cal`
- Высокоуровневые маршруты start/attach/stop/reset, чтобы .NET-клиент не собирал последовательность `-sto/-rst/-sta` сам
- Read/write маршруты для device-side конфигурации прибора: mode, state, noise, calibration registers
- Опциональную серверную компрессию `8192 -> 1024/2048/4096` каналов по той же логике, что сейчас в `AtomSpectraVCPIn`

## Что добавлено после проверки `atomspectra`

- `GET /api/v1/device/noise`
- `PUT /api/v1/device/noise`
- `POST /api/v1/device/mode`
- `GET /api/v1/device/calibration`
- `PUT /api/v1/device/calibration`
- `GET /api/v1/measurement/state`

Важно:
`POST /api/calibration` из текущей прошивки не покрывает Android-кейс записи калибровки в сам прибор.
Он меняет калибровку только внутри ESP32 gateway, а Android-приложение работает именно с device-side `-cal` registers.
