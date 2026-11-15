*[English](LID_SUSPEND.md) · Русский · [Español](LID_SUSPEND.es.md)*

# Крышка перестала усыплять машину

Как понять по журналу, кто усыпил машину, и где искать, когда крышка перестаёт
это делать. Демон здесь ни при чём: в его коде нет ни обработки крышки, ни
вызовов засыпания, ни блокировок.

## Как читать журнал logind

Два повода уснуть выглядят в `journalctl -u systemd-logind` по-разному, и по
журналу их можно различить только так:

- `Lid closed.`, а за ней `Suspending...` — крышка;
- `The system will suspend now!` — вызов `Suspend()` по шине (простой, меню
  сеанса).

Ловушка: при выходе из s2idle ядро заново сообщает состояние крышки, и в журнале
появляется пара `Lid closed.` / `Lid opened.` **в одну и ту же секунду**. Это не
закрытие крышки. Сверять с ядерными строками `PM: suspend entry` /
`PM: suspend exit`: такая пара стоит ровно на `exit`.

## Исходное состояние: logind с настройками по умолчанию

На этой машине logind обрабатывает крышку как из коробки:

- `logind.conf` пуст, каталогов с дополнениями нет;
- `HandleLidSwitch=suspend`, `HandleLidSwitchDocked=ignore`;
- устройство крышки помечено `power-switch`, и logind пишет
  `Watching system buttons on /dev/input/event0 (Lid Switch)`.

Исправное закрытие выглядит так: `Lid closed.` -> `Suspending...` ->
`PM: suspend entry (s2idle)`, а `LidClosed` у logind меняется одновременно с
`/proc/acpi/button/lid/LID/state`.

## Признак и первые проверки

Признак: при закрытии крышки в журнале logind нет строки `Lid closed.`. Пока
крышка в том состоянии, о котором идёт речь, выполнить:

```
systemd-inhibit --list | grep handle-lid-switch
busctl get-property org.freedesktop.login1 /org/freedesktop/login1 \
  org.freedesktop.login1.Manager HandleLidSwitch LidClosed
cat /proc/acpi/button/lid/LID/state
```

Если ACPI показывает `closed`, а logind молчит, событие до logind не доходит, и
вопрос в том, кто держит устройство ввода. Состояние блокировок в журнал не
пишется и перезагрузку не переживает, поэтому снимать его надо, пока сбой есть.

## Где искать

- **Композитор.** `cosmic-comp` умеет забирать крышку у logind: в его бинарнике
  есть строки `Inhibiting lid switch`, `Removing inhibitor-lock on lid switch` и
  `handle-lid-switch`. Он же держит открытым `/dev/input/event0` и обращается к
  `com.system76.PowerDaemon` — имени, на котором отвечает этот демон. Это след,
  а не подтверждённая причина: блокировку, взятую композитором, и надо искать
  первой проверкой выше.
- **Не `system76-power`.** В его пакете бинарник, юнит, настройка D-Bus,
  описание интерфейса и политика polkit — ни правил udev, ни дополнений к
  logind, ничего про крышку. Маскировка его юнита на крышку не влияет.
- Сырые события `/dev/input/event0` читает только `root` или член группы
  `input`.
