# Закреплённые инструменты и библиотеки

| Компонент | Версия / точный идентификатор | Применение |
| --- | --- | --- |
| ESP-IDF | v5.5.5, `b774170ff46c393eeb5e495ea37936038d3f4f4f` | SDK, FreeRTOS, lwIP, W5500, HTTPS, mbedTLS, FatFS |
| Docker SDK | `espressif/idf:v5.5.5@sha256:a9231d0697ab8f7517cc072e93b7c83e04907bfbfba80b6440d7dbbf90665cf2` | Воспроизводимая сборка |
| wolfSSH | v1.6.0-stable, `8221e4ee48c93da6cea3325a91de2ecc00ae5f56` | SSH-сервер |
| wolfSSL / wolfCrypt | v5.9.4-stable, `3c5eead44904df64e6a5a1f4ebdce377d35a849a` | Криптография для wolfSSH, без TLS-стека wolfSSL |
| Playwright | 1.63.0 | Необязательный браузерный тест; не входит в прошивку |

Исходники SDK: [ESP-IDF v5.5.5](https://github.com/espressif/esp-idf/tree/v5.5.5).
Локальные CMake-адаптеры и `user_settings.h` подключают исходники wolfSSL/wolfSSH
без изменения их репозиториев. Ускорители ESP32 в wolfCrypt отключены: используется
программная криптография; HTTPS отдельно использует mbedTLS из SDK. wolfSSH работает
в одной задаче, поэтому его конфигурация `SINGLE_THREADED` не означает, что всё
приложение однопоточное. Встроенное предупреждение wolfSSL для Xtensa о выборе
малых реализаций Curve25519/Ed25519 ожидаемо; библиотека выбирает их автоматически.

Лицензии поставщиков сохраняются в скачанных зависимостях:

- [wolfSSH LICENSING](https://github.com/wolfSSL/wolfssh/blob/8221e4ee48c93da6cea3325a91de2ecc00ae5f56/LICENSING): GPLv3 либо коммерческая лицензия.
- [wolfSSL LICENSING](https://github.com/wolfSSL/wolfssl/blob/3c5eead44904df64e6a5a1f4ebdce377d35a849a/LICENSING): GPLv3 либо коммерческая лицензия; перечисленное там исключение GPLv2 не относится автоматически к этому проекту.
- [ESP-IDF LICENSE](https://github.com/espressif/esp-idf/blob/v5.5.5/LICENSE): Apache-2.0, отдельные компоненты имеют собственные лицензии.

Лицензия собственных исходников проекта пользователем ещё не выбрана. Прототип
не подразумевает автоматического разрешения распространять закрытую прошивку с
GPL-компонентами; перед распространением необходимо определить лицензионную модель.
