*[English](LID_SUSPEND.md) · [Русский](LID_SUSPEND.ru.md) · Español*

# Cuando cerrar la tapa deja de suspender la máquina

Cómo saber por los registros quién suspendió la máquina, y dónde buscar cuando la
tapa deja de hacerlo. El demonio no interviene en esto: su código no maneja la
tapa, no pide suspender y no toma bloqueos.

## Cómo leer el registro de logind

Los dos motivos para suspender se ven distintos en
`journalctl -u systemd-logind`, y es la única forma de distinguirlos en el
registro:

- `Lid closed.` seguido de `Suspending...` — la tapa;
- `The system will suspend now!` — una llamada a `Suspend()` por el bus
  (inactividad, el menú de la sesión).

Una trampa: al salir de s2idle el núcleo vuelve a informar del estado de la tapa,
y el registro muestra un par `Lid closed.` / `Lid opened.` **en el mismo
segundo**. Eso no es un cierre de la tapa. Hay que compararlo con las líneas del
núcleo `PM: suspend entry` / `PM: suspend exit`: ese par cae justo en `exit`.

## Punto de partida: logind con la configuración de fábrica

En esta máquina logind gestiona la tapa con sus valores por defecto:

- `logind.conf` está vacío y no hay directorios de complementos;
- `HandleLidSwitch=suspend`, `HandleLidSwitchDocked=ignore`;
- el dispositivo de la tapa lleva la etiqueta `power-switch`, y logind indica
  `Watching system buttons on /dev/input/event0 (Lid Switch)`.

Un cierre correcto se lee así: `Lid closed.` -> `Suspending...` ->
`PM: suspend entry (s2idle)`, y `LidClosed` en logind cambia a la vez que
`/proc/acpi/button/lid/LID/state`.

## Síntoma y primeras comprobaciones

La señal: al cerrar la tapa no aparece la línea `Lid closed.` en el registro de
logind. Con la tapa todavía en el estado en cuestión, ejecutar:

```
systemd-inhibit --list | grep handle-lid-switch
busctl get-property org.freedesktop.login1 /org/freedesktop/login1 \
  org.freedesktop.login1.Manager HandleLidSwitch LidClosed
cat /proc/acpi/button/lid/LID/state
```

Si ACPI dice `closed` y logind no dice nada, el evento no llega a logind, y la
pregunta es quién tiene el dispositivo de entrada. El estado de los bloqueos no
se escribe en el registro ni sobrevive a un reinicio, así que hay que capturarlo
mientras el fallo está presente.

## Dónde buscar

- **El compositor.** `cosmic-comp` puede quitarle la tapa a logind: su binario
  contiene `Inhibiting lid switch`, `Removing inhibitor-lock on lid switch` y
  `handle-lid-switch`. También mantiene abierto `/dev/input/event0` y habla con
  `com.system76.PowerDaemon`, un nombre en el que responde este demonio. Es una
  pista, no una causa confirmada: un bloqueo tomado por el compositor es lo que
  hay que buscar con la primera comprobación de arriba.
- **No es `system76-power`.** Su paquete trae un binario, una unidad, una
  configuración de D-Bus, una descripción de la interfaz y una política de
  polkit: ni reglas de udev, ni complementos para logind, nada sobre la tapa.
  Enmascarar su unidad no afecta a la tapa.
- Los eventos en bruto de `/dev/input/event0` solo los puede leer `root` o un
  miembro del grupo `input`.
