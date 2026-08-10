*[English](HARDWARE.md) · [Русский](HARDWARE.ru.md) · Español*

# Superficie de control

Qué se puede controlar en esta plataforma, qué no, y qué hacen los controles en
realidad. Todos los valores de este documento se leyeron de la máquina en
marcha; lo que no está verificado se indica como tal.

Plataforma: ASUS Zenbook 14 UX3405MA, Intel Core Ultra 9 185H (22 hilos,
4.8 GHz máximo), Pop!_OS con kernel 7.0.11 y el escritorio COSMIC.
Controladores `asus_wmi` / `asus_nb_wmi` / `asus_armoury`; el
`platform_profile` de ACPI funciona. `system76-power` y `thermald` se ejecutan
de forma predeterminada; `power-profiles-daemon` no está instalado.

## Lo que no está disponible

Dejarlo claro desde el principio evita volver a deducirlo más adelante.

- **No hay control manual del ventilador.** `pwm1_enable` solo acepta `2`
  (automático, gobernado por el EC) y `0` (velocidad máxima). No existe ningún
  nodo con el valor de PWM ni ninguna curva del ventilador en ninguna parte:
  `/sys/class/firmware-attributes/asus-armoury/attributes/` contiene una sola
  entrada, `pending_reboot`, y este firmware no expone ni curvas del ventilador
  ni atributos PPT. En la velocidad del ventilador solo se puede influir de
  forma indirecta, cambiando la cantidad de calor que se produce o eligiendo
  otra curva del EC con `platform_profile`.
- **No se puede reducir el voltaje.** Intel lo bloquea en Meteor Lake.
- **`asusctl` sin probar.** Aquí no está instalado y el soporte para el
  UX3405MA está sin verificar.

## Perfil de política térmica

- `/sys/firmware/acpi/platform_profile`: `quiet` / `balanced` / `performance`;
  el conjunto se confirma leyendo `platform_profile_choices`.
- `/sys/devices/platform/asus-nb-wmi/throttle_thermal_policy`: 0 = balanced,
  1 = performance, 2 = quiet. Sigue a `platform_profile`.
- `system76-power profile balanced|performance` cambia los dos sin root. No
  tiene ajuste `quiet`; para eso hay que escribir en sysfs directamente como
  root.

Esto elige la curva del ventilador del EC y es la única manera de desplazar la
curva entera. Con `quiet` el EC tolera más calor antes de acelerar, y el chasis
queda más caliente a cambio de un ventilador más lento.

**Al escribir `platform_profile`, el firmware vuelve a imponer sus propios
límites de potencia.** Por eso cualquier valor de RAPL se escribe *después* del
perfil, nunca antes, y se comprueba de nuevo al terminar.

## Ventilador

- Directorio de hwmon: `/sys/devices/platform/asus-nb-wmi/hwmon/hwmon*/`. El
  índice `hwmon*` no se mantiene entre arranques: hay que resolverlo por `name`
  (`asus`).
- `fan1_input`: velocidad en RPM.
- `pwm1_enable`: `2` automático, `0` velocidad máxima. Requiere root.
- El hwmon `acpi_fan` expone una segunda lectura de la velocidad (`fan1`), que
  sirve de contraste.

Comportamiento medido en una muestra de 12 minutos a 0.2 Hz, con una carga
media en torno a 0.6:

| | |
|---|---|
| Velocidad en reposo | 1900 RPM en 109 de 144 muestras |
| Rango observado | 1900–2500 RPM en reposo; 3200 con carga ligera; 6100 como máximo |
| Umbral de aceleración | sube por encima del mínimo con el paquete en torno a 62 °C |

### 1900 RPM no es el mínimo

En la muestra corta de arriba el ventilador nunca aparece parado, y por eso
1900 RPM parecía un mínimo del hardware. Dos sesiones grabadas a 1 Hz dicen lo
contrario: 6019 muestras y 20 204 muestras, tomadas con un día de diferencia:

| Paquete, °C | Muestras con `fan1_input` = 0 | Ventilador p50 / p90, RPM |
|---|---|---|
| por debajo de 45 | 100 % | 0 / 0 |
| 45–48 | 29 % | 1700 / 1700 |
| 48–50 | 16 % | 1700 / 1900 |
| 50–52 | 3 % | 1900 / 2100 |
| 52–55 | 0.4 % | 2000 / 2000 |
| 55–60 | 0 % | 2000 / 2800 |
| por encima de 60 | 0 % | 3000 / 3700 |

Por debajo de unos 45 °C el EC detiene el ventilador del todo; entre 45 y 52 °C
lo mantiene al borde de pararse; por encima de 55 °C no lo detiene nunca. Aquí
un cero es una lectura y no un hueco: una lectura que falta se registra como
ausente, y la exportación la escribe como un campo vacío.

Para el objetivo de ruido esto es mejor que un mínimo fijo, y cambia lo que se
puede conseguir ajustando el reposo: al final sí queda velocidad de ventilador
que ceder, pero solo produciendo menos calor, que es el mismo control de antes.

### El chasis y el paquete van a la par

En las dos sesiones la lectura de `acpitz` y la temperatura del paquete
coinciden con menos de un grado de diferencia en todas las bandas de carga, así
que con el portátil sobre la mesa la temperatura del paquete representa bien la
del chasis. Esto no vale para un portátil apoyado encima de una persona: el
camino del calor es otro, y un escenario que se juzga por la piel se juzga con
`acpitz`.

## Potencia y frecuencia de la CPU

- `/sys/devices/system/cpu/intel_pstate/no_turbo`: con 1 se desactiva el turbo.
- `/sys/devices/system/cpu/intel_pstate/max_perf_pct`: techo de frecuencia en
  porcentaje (aquí `min_perf_pct` es 8).
- `/sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost`: 1 de forma
  predeterminada.
- `/sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference`:
  `default` / `performance` / `balance_performance` / `balance_power` /
  `power`. El valor predeterminado es `balance_performance`.
- `scaling_driver` es `intel_pstate` y `scaling_governor` es `powersave`.

Entre el EPP predeterminado y el HWP dynamic boost, el paquete sube casi a la
frecuencia máxima por cualquier trabajo trivial. Esos picos cortos son los que
calientan el paquete cuando la máquina está en teoría en reposo; el límite de
potencia prolongado no se alcanza en reposo y por lo tanto no influye.

### RAPL

`/sys/class/powercap/intel-rapl:0/` (`name` = `package-0`):

| Nodo | Significado | Observado |
|---|---|---|
| `constraint_0_power_limit_uw` | PL1, límite prolongado, µW | 200 W, tal como los fija el perfil performance del firmware |
| `constraint_1_power_limit_uw` | PL2, límite breve, µW | 115 W |
| `constraint_2_power_limit_uw` | potencia de pico | 217 W |
| `constraint_0_time_window_us` | ventana de promediado de PL1 | 31 981 568 µs (≈32 s) |
| `constraint_1_time_window_us` | ventana de promediado de PL2 | 2440 µs |
| `energy_uj` | contador de energía del paquete, solo root | da la vuelta en `max_energy_range_uj` |

Los subdominios `intel-rapl:0:0` (`core`) e `intel-rapl:0:1` (`uncore`) llevan
contadores de energía propios, y eso permite distinguir el calor de la CPU del
de la iGPU.

PL1 es el control más eficaz para la carga sostenida. La ventana de tiempo de
PL1 se puede escribir y se aprovecha poco: una ventana corta da una potencia
constante en lugar de picos, y así baja la temperatura máxima a un coste casi
imperceptible.

**Un PL1 de 200 W en un portátil pensado para unos 28 W es más de lo que el
chasis puede disipar.** Con carga sostenida el paquete llega a alcanzar el corte
del hardware antes de que el bucle de limitación reaccione; el registro del
kernel termina entonces a mitad de línea, sin ningún apunte de apagado, y esa es
la firma de un THERMTRIP y no de un fallo de software. Este es el modo de fallo
que el proyecto existe para evitar.

## GPU integrada

Controlador `i915`, expuesto como `card1`. Una videollamada carga el motor
gráfico tanto como los núcleos: decodifica cada flujo remoto, codifica el local
y ejecuta cualquier inferencia continua, como el encuadre automático. **Ninguno
de los controles del lado de la CPU influye en eso.** `max_perf_pct`,
`no_turbo` y el EPP actúan solo sobre los núcleos. El PL1 del paquete sí limita
el total, pero el motor gráfico tiene además controles propios:

| Nodo | Significado | Observado |
|---|---|---|
| `/sys/class/drm/card*/gt_max_freq_mhz` | techo de frecuencia, se puede escribir | 2350 |
| `/sys/class/drm/card*/gt_min_freq_mhz` | frecuencia mínima | 100 |
| `/sys/class/drm/card*/gt_act_freq_mhz` | frecuencia actual | — |
| `/sys/class/drm/card*/gt_RP0_freq_mhz` | máximo del hardware | 2350 |
| `/sys/class/drm/card*/gt_RPn_freq_mhz` | mínimo del hardware | 800 |
| `/sys/class/powercap/intel-rapl:0:1/constraint_0_power_limit_uw` | límite de potencia del dominio uncore | — |

**El valor más bajo que acepta `gt_max_freq_mhz` lo fija `gt_RPn_freq_mhz`, que
aquí es 800, y no `gt_min_freq_mhz`.** Los dos nodos se confunden con facilidad
y la diferencia es silenciosa: un techo por debajo de RPn se rechaza sin más, el
nodo conserva su valor anterior y nada lo dice salvo volver a leerlo. Se midió
por las malas: en 3826 muestras consecutivas con un techo pedido de 100, todas
informaron de que el control no coincidía con el demonio, y en ninguna estaba
aplicado.

Con estos controles se puede limitar el motor gráfico sin tocar el rendimiento
de la CPU, lo que importa cuando el calor nace ahí.

El controlador multimedia de Intel (`iHD_drv_video.so`) está instalado, así que
la decodificación y la codificación de vídeo por hardware están disponibles en
el sistema. Que una aplicación concreta las use es otra cuestión: decodificar
por software muchos flujos de vídeo a la vez sale caro y se puede evitar por
completo.

### Conectores de pantalla

`/sys/class/drm/card1-<connector>/status` indica `connected` o `disconnected`
para cada conector, sin sesión y sin servidor gráfico, y por eso el demonio
puede contarlos. Aquí se observan `eDP-1` para el panel interno y `DP-1`,
`DP-2` y `HDMI-A-1` para los conectores externos.

El número queda registrado en cada muestra porque mover un monitor externo es
trabajo que no se puede aplazar: un techo de la iGPU no lo retrasa, solo hace
que llegue tarde. Y su coste no se ve en un registro que no dice si había un
monitor conectado.

Lo que el nodo no dice: un conector marca `connected` mientras tenga un cable
puesto, esté encendida la salida o no, y un concentrador DP-MST no aparece como
cabría esperar. La resolución y el escalado no están ahí en absoluto: el
conector enumera los modos que admite, y cuál está en uso lo sabe el
compositor. En cualquier caso los dos son un indicio de cuánto trabajo gráfico
se está haciendo, y eso ya se mide de forma directa con el dominio uncore de
RAPL y con `gt_act_freq_mhz`.

### Los monitores externos responden a DDC/CI; el panel interno, no

El brillo de un monitor externo no está en sysfs. Es una conversación por el bus
I2C de su conector, en la dirección 0x37 —DDC/CI—, y el bus está en
`/sys/class/drm/card1-<conector>/ddc` como enlace simbólico al `i2c-N` por el que
va el intercambio. Medido aquí: `DP-1` es el bus 12 y `DP-2` el 13, y el bus
pertenece al conector y no al monitor.

Se midieron tres monitores, todos por la característica VCP 0x10 y todos
declarando un máximo de 100:

| Monitor | Conector, bus | Serie en EDID | Lectura | Escritura, con la verificación de la biblioteca | Escritura, sin ella |
|---|---|---|---|---|---|
| `DELL U3425WE`, MCCS 2.1 | `DP-1`, `i2c-12` | `421B3Z3` | 353 ms | 405 ms, éxito | 301 ms, éxito |
| `RTK TYPEC`, MCCS 2.2 | `DP-2`, `i2c-13` | `L56051794302` | 351–357 ms | **1344 ms, DDCRC_RETRIES** | 302 ms, éxito |
| `WCS AQ14T` | `DP-2`, `i2c-13` | ninguna, y `0x01010101` en el campo numérico | 351–359 ms | 409 ms, éxito | 300 ms, éxito |

De ahí se siguen cuatro cosas, y las cuatro están en el código:

- **La verificación propia de la biblioteca rompe la escritura.** `ddcutil` vuelve
  a leer el valor de inmediato, y uno de estos paneles no está listo para
  responder tan pronto: informa de `DDCRC_RETRIES` mientras el valor que se le dio
  ya está en el monitor. Con la verificación apagada la misma escritura cuesta
  302 ms e informa de éxito, así que está apagada y lo que confirma es la
  siguiente lectura ordinaria.
- **El código de retorno de una escritura no es la respuesta.** Lo es la lectura.
  El fallo de arriba está medido, no supuesto.
- **Los botones del propio monitor se ven.** Un nivel puesto con sus teclas se lee
  por DDC, y eso es lo que permite que el modelo «una mano es una indicación»
  funcione igual para un monitor que para el panel.
- **De la serie no se puede depender.** Un panel no trae descriptor de serie en
  absoluto y lleva un relleno en el campo numérico, así que donde no hay serie el
  conector ocupa su lugar en el identificador.

El panel interno está en el bus 11 y responde `DDC communication failed`, diciendo
sin rodeos que un panel de portátil no soporta DDC/CI. Se queda en
`intel_backlight`.

Dos monitores leídos en secuencia cuestan 704–707 ms, así que los buses se suman en
vez de solaparse, y todo el intercambio vive en un hilo propio: en el bucle de 1 Hz
está la protección de emergencia, y no espera a una pantalla.

`/dev/i2c-*` es `root:i2c 0660`, con una ACL para el usuario local del asiento. El
demonio es root y no necesita nada más.

### Contadores de limitación del hardware

`/sys/devices/system/cpu/cpu*/thermal_throttle/`: `core_throttle_count`,
`package_throttle_count`, `*_total_time_ms`, `*_max_time_ms`. Cuentan la
limitación de emergencia del propio hardware, así que un contador que sube es
una señal directa de que el margen fijado por software ha fallado. Los contadores
son acumulativos desde el arranque, y lo único que significa algo es su
velocidad de cambio.

## Batería

`/sys/class/power_supply/BAT0/charge_control_end_threshold` existe y se puede
escribir, así que limitar la carga es cuestión de un solo archivo.

## Sensores

| `name` del hwmon | Proporciona |
|---|---|
| `coretemp` | `Package id 0` y las temperaturas de cada núcleo; high = crit = 110 °C |
| `acpitz` | temperatura del chasis o de la placa |
| `nvme` | Composite y otros dos sensores; crit 84.8 °C |
| `iwlwifi_1` | temperatura del módulo Wi-Fi |
| `BAT0` | potencia de la batería |
| `asus` | velocidad del ventilador |
| `acpi_fan` | velocidad del ventilador, segunda fuente |

Los índices `hwmon*` no se mantienen entre arranques. Cada sensor se resuelve
por `name`.

## Sensor de luz ambiental y retroiluminación

Los dos existen y son independientes de todo lo anterior. Están documentados
aquí porque el demonio escribe la retroiluminación, no porque ninguno tenga que
ver con el calor: elegir un modo térmico nunca mueve la retroiluminación.

| Nodo | Valor |
|---|---|
| `/sys/bus/iio/devices/iio:device0/name` | `als`; solo sirve para identificarlo, ver más abajo |
| `.../in_illuminance_raw` | sube con la luz; se puede leer sin root |
| `.../in_illuminance_scale` | `0.001000000`; las cuentas en bruto son milésimas de lux. Un controlador no tiene obligación de ofrecer este nodo, y entonces el demonio toma la escala como 1 |
| `.../in_illuminance_offset` | `0`; se suma a la cuenta en bruto antes de aplicar la escala |
| `.../in_illuminance_sampling_frequency` | `10.0` |
| `/sys/class/backlight/intel_backlight/max_brightness` | 400 |
| `/sys/class/backlight/intel_backlight/brightness` | `root:root 0644` |

Dos propiedades deciden cómo los usa el demonio.

**La lectura está en lux, y el controlador lo dice.** `(raw + offset) × scale`
es el contrato de IIO para `in_illuminance`, y los números que salen de ahí
concuerdan con la luz que hay en la habitación. Medido en esta máquina, en una
sola tarde:

| Habitación | Lux |
|---|---|
| a oscuras, con la pantalla apagada | 0.3 |
| dos lámparas de escritorio y una tira de led, sin luz de techo | 25.9 |
| lo mismo con la luz de techo encendida: cuatro bombillas de 1000 lm, el portátil en una esquina y de cara a ellas | 65.5 a 103.5 |
| una luz de bicicleta de 1800 lm a unos 7 cm del sensor | 20 855 |

El demonio guarda la lectura en mililux, porque una habitación a oscuras vive
en la parte baja de ese rango y con lux enteros no se distingue 0.3 de 3. La
curva de respuesta se configura en lux, en el archivo de ajustes y en la línea
de comandos, y se calibra con telemetría grabada.

El extremo alto del rango del sensor no está determinado. No hubo a mano nada
más brillante que la luz de bicicleta y no hay ninguna sesión grabada con luz
de día, así que el último punto de la curva se apoya en el rango observado y no
en un techo medido.

**La retroiluminación solo la puede escribir root.** Por eso es del demonio y la
bandeja no la toca: todo lo que la bandeja hace con la retroiluminación pasa por
el demonio. Cualquier otro proceso que se ejecute como root también puede
escribirla, y de ahí que el demonio recuerde el último nivel que conoce: una
lectura distinta de lo que recuerda es la escritura de otro, y en la práctica
ese otro es la persona pulsando las teclas de brillo.

`iio-sensor-proxy` está instalado pero no se está ejecutando, así que ahora
mismo nada más gobierna la retroiluminación a partir del sensor. Si se
arrancara, él y este demonio serían dos dueños de un mismo control, que es
justo lo que este proyecto existe para evitar.

El índice del dispositivo iio es igual de inestable que el de un `hwmon`. El
demonio encuentra el sensor por la presencia de un canal de iluminancia, no por
el índice ni por `name`, porque los controladores no se ponen de acuerdo en qué
escribir en `name` y un dispositivo con un canal de iluminancia es un sensor de
luz, se llame como se llame.

## Comportamiento térmico medido

Medido en una sola máquina. Los valores absolutos dependen del estado de la
refrigeración de cada unidad; lo que se traslada a otras son las relaciones
entre los ajustes.

Cómo se midió, porque un número sin su método no es una medida: ocho hilos en
bucle ocupado, **90 s de estabilización** para cada combinación antes de
**90 s de registro**, y la máquina enfriada de nuevo a **65 °C entre
combinaciones** para que ninguna herede el calor de la anterior. El tiempo de
estabilización no es arbitrario: la constante de tiempo del disipador es de
decenas de segundos, y muestrear antes mide el transitorio en lugar del estado.
La ejecución abortaba la combinación en curso si el paquete llegaba a 95 °C. Las
cifras en reposo salen del mismo procedimiento con cero hilos de carga.

- La temperatura del paquete se estabiliza en **60–63 °C en reposo**, con el
  ventilador en su mínimo y la carga media por debajo de 1.
- Pasar del perfil `balanced` del fabricante a `battery` (techo de frecuencia
  recortado del 100 % al 50 %, turbo desactivado, curva del EC en `quiet`)
  cambió la temperatura del paquete en un grado escaso y dejó la velocidad del
  ventilador igual. Dos efectos se anulan: se produce menos calor y la curva
  más perezosa quita exactamente ese mismo caudal de aire. Suprimir los picos,
  por sí solo, no aporta nada.
- Los perfiles del fabricante no tocan RAPL en absoluto. Los límites de
  potencia escritos a mano sobreviven intactos a cualquier cambio de perfil.

## Conflictos de propiedad

- `system76-power` es dueño de los tres nombres de D-Bus a la vez:
  `com.system76.PowerDaemon`, `net.hadess.PowerProfiles` y
  `org.freedesktop.UPower.PowerProfiles`. `power-profiles-daemon` no está
  instalado, así que nadie más compite por los dos últimos. El archivo de
  políticas que viene de serie ya permite que cualquier proceso de root los
  tome, y ninguno de los tres se puede activar por bus: no hay archivo de
  activación en `/usr/share/dbus-1/system-services/`. Por eso enmascarar la
  unidad traspasa la propiedad de forma limpia y nada rearranca el demonio del
  fabricante cuando alguien lo pide.
- **El nombre real de la unidad del fabricante es
  `com.system76.PowerDaemon.service`**; `system76-power.service` es un `Alias=`
  que declara esa unidad, y en el disco el alias solo existe como un enlace
  simbólico a ella. Enmascarar el alias sustituye ese enlace y deja la unidad
  real activada en `multi-user.target.wants`, con lo que el demonio del
  fabricante vuelve en el siguiente arranque. Todo lo que la enmascare, la
  desenmascare o entre en conflicto con ella tiene que nombrar la unidad real.
- `system76-power` además ajusta la retroiluminación de la pantalla al cambiar
  de perfil.
- `thermald` se ejecuta de forma predeterminada y escribe en RAPL y en
  `intel_pstate` por su cuenta.
- `system76-scheduler` ajusta las prioridades de los procesos y no tiene nada
  que ver con lo térmico.

Todas las escrituras en sysfs se pierden al reiniciar.

### La interfaz D-Bus del fabricante

Quedarse con el nombre obliga a responder a todo lo que hay en él. Según la
introspección hecha en esta máquina, `com.system76.PowerDaemon` son dieciséis
métodos y dos señales:

| Miembro | Firma | Nota |
|---|---|---|
| `Battery` / `Balanced` / `Performance` | `() → ()` | eligen un perfil |
| `GetProfile` | `() → s` | |
| `GetChargeThresholds` | `() → (y, y)` | porcentaje de inicio y de fin |
| `SetChargeThresholds` | `((yy)) → ()` | |
| `GetChargeProfiles` | `() → a(sssyy)` | id, título, descripción, inicio, fin |
| `GetGraphics` / `SetGraphics` | `() → s` / `(s) → ()` | |
| `GetGraphicsPower` / `SetGraphicsPower` | `() → b` / `(b) → ()` | |
| `AutoGraphicsPower` | `() → ()` | |
| `GetDefaultGraphics` | `() → s` | |
| `GetSwitchable` | `() → b` | |
| `GetDesktop` | `() → b` | |
| `GetExternalDisplaysRequireDgpu` | `() → b` | |
| `HotPlugDetect` | señal `(t)` | |
| `PowerProfileSwitch` | señal `(s)` | |

Once de ellos tienen que ver con la conmutación de gráficas, algo que esta
máquina no puede hacer: no hay GPU dedicada. Son constantes, no funcionalidad.

El vocabulario es el del fabricante y no el estándar: `GetProfile` y
`PowerProfileSwitch` hablan de `Battery`, `Balanced` y `Performance`, con
mayúscula inicial. La forma de escribirlo es parte del contrato: el applet del
panel de COSMIC compara la respuesta con esos tres literales y aborta con
cualquier otra cosa, y un applet abortado se lleva del panel el indicador de
batería junto con el menú de perfiles.

La interfaz estándar, `net.hadess.PowerProfiles` con su alias
`org.freedesktop.UPower.PowerProfiles`, es la más pequeña: las propiedades
`ActiveProfile`, `Profiles`, `Actions`, `ActiveProfileHolds`,
`PerformanceInhibited` y `PerformanceDegraded`, los métodos `HoldProfile` y
`ReleaseProfile`, y la señal `ProfileReleased`. Acepta exactamente tres nombres
de perfil: `power-saver`, `balanced` y `performance`.

## Diagnóstico

```bash
sensors                                              # temperatures and fan speed
journalctl -k -b -1 | grep -i therm                  # throttling in the previous boot
journalctl --list-boots                              # locate truncated sessions
cat /sys/firmware/acpi/platform_profile              # current profile
grep . /sys/class/powercap/intel-rapl:0/constraint_*_power_limit_uw
grep . /sys/devices/platform/asus-nb-wmi/hwmon/hwmon*/fan1_input
busctl --system introspect com.system76.PowerDaemon /com/system76/PowerDaemon
```
