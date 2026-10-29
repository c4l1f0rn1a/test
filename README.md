# SipPhone

Лёгкий SIP-телефон для Linux в духе MicroSIP: тонкая оболочка на GTK3
поверх PJSIP, всё на C.

## Что умеет (MVP)

- один SIP-аккаунт (UDP / TCP / TLS, SRTP, STUN, прокси)
- номеронабиратель, вызов, ответ, сброс
- всплывающее окно и уведомление о входящем, рингтон и гудки
- удержание, отключение микрофона, DTMF (RFC 4733, запасной — SIP INFO)
- громкость микрофона и динамика
- статус регистрации, иконка в трее (KDE Plasma и другие с SNI)
- звонки по ссылкам `sip:`, `tel:`, `callto:` и из командной строки
- настройки в одном ini-файле

Кодеки: Opus, G.722, G.711 (PCMA/PCMU). Пока одна линия: второй
входящий во время разговора получает «занято».

## Сборка

Зависимости (Debian 12+ / Ubuntu 24.04+):

```sh
sudo apt install build-essential pkg-config git \
    libgtk-3-dev libayatana-appindicator3-dev \
    libasound2-dev libopus-dev libssl-dev uuid-dev
```

PJSIP собирается статически и вшивается в бинарник:

```sh
make pjsip     # один раз: скачивает и собирает PJSIP в third_party/
make
./sipphone
```

Установка в систему (с `.desktop`-файлом и обработчиком `sip:`/`tel:`):

```sh
sudo make install
```

## Запуск

```
sipphone [ОПЦИИ] [НОМЕР | sip:URI | tel:НОМЕР]

  -m, --minimized   запуститься свёрнутым в трей
  -d, --debug N     уровень логов PJSIP в консоли (0-6)
  --null-audio      без звуковой карты (для тестов)
```

Программа запускается в одном экземпляре: `sipphone 101` при уже
открытом окне звонит через него.

## Настройки

`~/.config/sipphone/sipphone.ini` (права `0600`, внутри пароль).
В AppImage portable-режиме — в папке `SipPhone.AppImage.config` рядом
с файлом.

## Лицензия

GPL-2.0-or-later (как и PJSIP).
