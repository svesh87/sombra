*[English](../README.md) · [Русский](README.ru.md) · Español*

# sombra

*Sombra*: a la sombra una máquina está más fresca y más apagada, y de eso se
ocupa este demonio.

Gestión del calor, del ruido y de la retroiluminación del ASUS Zenbook 14
UX3405MA (Intel Core Ultra 9 185H) en Linux: un demonio en segundo plano que
mantiene la máquina dentro de un margen térmico elegido y sigue al sensor
de luz ambiental, y una consola en la bandeja para cambiar de margen y
afinarla de oído.

## Por qué existe

Tres características de esta plataforma se llevan mal entre sí:

- **El firmware no ofrece ningún control del ventilador.** `pwm1_enable` acepta
  solo *automático* y *velocidad máxima*; no hay valor manual de PWM ni curva
  del ventilador en ninguna parte, ni en hwmon ni en `asus-armoury`. Las
  revoluciones no son, por tanto, algo que el software pueda fijar. Solo son
  algo que el software puede *provocar*, cambiando cuánto calor genera el
  paquete.
- **El perfil de rendimiento del firmware quita el límite de potencia.**
  Programa el PL1 de RAPL a 200 W en un portátil dimensionado para unos 28 W,
  más de lo que el chasis puede disipar y suficiente para llegar al apagado
  térmico por hardware antes de que el sistema operativo pueda reaccionar.
- **Las herramientas del fabricante juntan mandos que deberían ir separados.**
  `system76-power` trae tres perfiles fijos, y cada uno fija a la vez el techo
  de frecuencia de la CPU, el turbo, la preferencia de energía, el perfil de
  plataforma de ACPI y la retroiluminación de la pantalla. La combinación más
  útil aquí, un tope de potencia firme con una curva del ventilador que *no* sea
  perezosa, no se puede expresar. Tampoco «limita la CPU y deja en paz el brillo
  de mi pantalla».

El resultado es una máquina que está caliente y silenciosa o fría y ruidosa, sin
manera de pedir el punto intermedio. Este proyecto da ese punto.

## Resumen del diseño

- Un único demonio es dueño de todos los controles térmicos y de potencia. Dos
  procesos escribiendo en los mismos nodos de sysfs no dan redundancia, sino un
  comportamiento que depende de quién llegue antes, así que `system76-power` se
  enmascara en vez de montar nada encima de él.
- El demonio responde en los nombres estándar de D-Bus
  `net.hadess.PowerProfiles` y `org.freedesktop.UPower.PowerProfiles`, y en
  `com.system76.PowerDaemon` por compatibilidad con el applet del panel de
  COSMIC. Así siguen funcionando tanto el panel del escritorio como la bandeja.
- Los márgenes se editan a través del demonio, desde la línea de órdenes o
  desde la bandeja, y el demonio las guarda en un archivo de texto propio. Se
  afinan analizando telemetría grabada, así que la herramienta que analiza tiene
  que poder aplicar lo que ha encontrado, sin root y sin manejar ningún diálogo.
- El demonio graba telemetría de los sensores una vez por segundo en un anillo
  de tamaño fijo que mantiene en memoria y vuelca al disco de tarde en tarde,
  para que las decisiones de ajuste salgan de datos y no de suposiciones. Cada
  muestra lleva los valores de consigna que estaban en vigor, porque un registro
  que no dice qué se aplicó no se puede analizar.
- Los valores de consigna son estáticos en cada modo. No hay controlador
  realimentado: las medidas mostraron que los valores estáticos sirven tanto en
  reposo como bajo carga sostenida, y un controlador añadiría coeficientes que
  afinar, además de fallos que los valores estáticos no pueden tener: oscilar
  alrededor del objetivo y quedarse pegado a un valor.
- Una protección térmica de emergencia está siempre activa, sea cual sea el
  modo. Solo baja, nunca sube, y es lo único que pone el ventilador en marcha
  por su cuenta. Es un enclavamiento de seguridad, no una regulación.
- La bandeja es una consola, no un diálogo de ajustes. Elija un modo y sus
  controles toman los valores del modo; mueva uno y se aplica al instante, y el
  selector de modo se queda vacío, porque lo que está en marcha ya no es ninguno
  de los tres. Un botón escribe los controles en el modo que usted nombre, a
  través del demonio y con las mismas órdenes que envía la línea de órdenes.
  Otros tres marcan un juicio en la muestra actual («bien», «demasiado ruidoso»,
  «demasiado lento»), para que una opinión hecha de oído siga siendo rastreable
  hasta los números que la produjeron.
- Elegir un modo nunca mueve la retroiluminación de la pantalla. Juntar la
  retroiluminación con el perfil de potencia es cosa del fabricante, y este
  proyecto lo trata como un defecto.
- Seguir al sensor de luz ambiental es una función aparte, con su propio
  interruptor. Trabaja alrededor del nivel que ha fijado la persona en vez de
  imponer uno propio: la habitación suma o resta a ese nivel, y una corrección a
  mano es una declaración nueva de lo que se quiere, no algo que haya que
  enmendar. Viene apagada hasta que se enciende, y esa elección sobrevive a un
  reinicio.

## Alcance

Construido y medido en una sola máquina. Todo lo que hay en `HARDWARE.md` se
verificó en:

| | |
|---|---|
| Portátil | ASUS Zenbook 14 UX3405MA |
| CPU | Intel Core Ultra 9 185H (Meteor Lake) |
| SO | Pop!_OS, kernel 7.0.11, escritorio COSMIC |
| Controladores | `asus_wmi` / `asus_nb_wmi` / `asus_armoury`, `platform_profile` de ACPI |

Otros portátiles de ASUS ofrecen un conjunto de controles distinto, y otros
escritorios son dueños de nombres de D-Bus distintos. Portar esto pasa por
volver a verificar antes la superficie de control: `HARDWARE.md` documenta qué
controles existen aquí y, algo igual de importante, cuáles no.

## Modos

Tres, del margen más frío al más amplio. El del medio es el modo de
trabajo: la máquina pasa allí la inmensa mayoría del tiempo, se aplica al
arrancar y es al que apunta el ajuste por telemetría.

| Modo | PL1/PL2 | Techo de frecuencia | Techo de iGPU | Turbo | EPP | Ventilador |
|---|---|---|---|---|---|---|
| `quiet` | 12/20 W | 40 % | 1000 MHz | desactivado | `power` | curva `quiet` |
| `work` | 20/30 W | 50 % | 2350 MHz | desactivado | `power` | curva `balanced` |
| `full` | 45/65 W | 100 % | 2350 MHz | activado | `balance_performance` | curva `balanced` |

El ventilador es un valor de consigna como los demás: un modo nombra una de las
tres curvas del firmware y puede pedir velocidad máxima. Lo que no debe ocurrir
es llegar ahí por su cuenta. Nada acelera el ventilador sin que se lo hayan
pedido, y la protección de emergencia es la única excepción.

## Qué hay aquí

Tres binarios, y el que se escribe es `sombra`.

| Binario | Para qué sirve |
|---|---|
| `sombrad` | el demonio: aplica el modo, sigue al sensor de luz, graba telemetría, responde en los nombres de D-Bus |
| `sombra` | control programable: elegir un modo, afinar en vivo, marcar un veredicto, leer el anillo de telemetría |
| `sombra-tray` | la consola de experimentación |

`sombra export` es un subcomando y no un programa aparte, y es el único que no
pasa por el demonio: el anillo es un archivo, el demonio no guarda historial, y
una semana de muestras no tiene nada que hacer viajando por un protocolo de
líneas de texto.

| Script | Para qué sirve |
|---|---|
| `scripts/install.sh` | compila, instala y entrega los controles térmicos |
| `scripts/rollback.sh` | se los devuelve al demonio del fabricante |
| `scripts/screenshots.sh` | hace las imágenes de la documentación |

La consola tiene un `?` al final de su línea de estado. Abre la referencia: cada
control de cada pestaña con sus unidades, su rango y lo que cuesta, más el modelo
sobre el que está construida la ventana, en el idioma en que habla la ventana.
Instalada como `/usr/share/sombra/help/sombra-help.<idioma>.html`.

El paquete instala páginas de manual, `sombra(1)`, `sombrad(8)` y
`sombra-tray(1)`, y autocompletado para bash, zsh y fish. El autocompletado saca
los verbos de un archivo estático y solo le pregunta al demonio los nombres de
los modos, porque un tabulador que abre un socket es un tabulador que se cuelga
cuando el demonio no está.

Los valores que los modos traen compilados en el demonio salen de medidas hechas
en una sola máquina y son un punto de partida, no un resultado. Están para
afinarse a partir de telemetría grabada, y para eso están `sombra export` y las
marcas subjetivas.

Cuánta telemetría se guarda es un ajuste, `retention_days`: de un día a treinta,
siete por omisión. La profundidad es el tamaño propio del anillo y no una política
aplicada a él, así que cambiarla reconstruye el archivo y arrastra las muestras, y
acortarla descarta las más antiguas. Con registros de 128 bytes eso son unos
10,5 MiB por día, y el archivo ocupa todo su tamaño desde que se crea. `enabled=off`
detiene solo el añadido: la muestra se sigue tomando, `status` sigue respondiendo y el
archivo se queda como está. Ambos están también en la pestaña *Ajustes* de la
consola.

Una versión que cambie la disposición de los registros convierte el anillo en el
siguiente arranque, campo a campo, y anota lo que el formato antiguo no tenía
como ausente y no como cero. Un archivo que esta compilación no sepa leer se
conserva como `metrics.ring.bak` y un anillo nuevo ocupa su lugar: una
disposición de una compilación más reciente tras una vuelta atrás, una que nunca
se publicó, o una cabecera dañada por un corte de luz. Nada se borra por ser
ilegible, y un anillo ilegible cuesta las muestras que contiene y nada más: el
demonio sigue gestionando la máquina.

Hay un solo espacio de copia de seguridad y cada vuelta atrás lo sobrescribe.
Si no, volver atrás dos veces dejaría un anillo por el camino en cada paso, y el
del medio no le sirve a nadie.

`scripts/install.sh` exporta además el anillo a CSV antes de sustituir nada, en
`$XDG_STATE_HOME/sombra/telemetry` (`--dump-dir` cambia el destino). El CSV
sobrevive a cualquier disposición, y eso la conversión de arriba no lo puede
prometer para un formato que nadie ha escrito todavía.

## Compilación e instalación

Necesita CMake 3.16, un compilador de C++17 y Qt 6 (Core, Network, DBus,
Widgets, Test, LinguistTools). En Debian y sus derivadas:

```bash
sudo apt install build-essential cmake \
    qt6-base-dev qt6-base-dev-tools libqt6svg6 \
    qt6-l10n-tools qt6-tools-dev \
    debhelper dpkg-dev lcov
```

`qt6-l10n-tools` y `qt6-tools-dev` son las herramientas de traducción:
`lupdate`, `lrelease` y el paquete de CMake que los maneja. `debhelper` y
`dpkg-dev` sirven para `make package`, y `lcov` para el umbral de cobertura;
compilar y ejecutar los binarios no necesita ninguno de ellos.

```bash
make            # compilar
make test       # el conjunto completo; ninguna prueba toca hardware real
make gates      # lint, pruebas, el umbral de cobertura y git diff --check
make package    # compila el .deb y comprueba lo que ha salido
```

Un solo script hace todo el despliegue: compilar, instalar, entregar los
controles y añadir su usuario al grupo. Compila con su usuario y eleva
privilegios solo en las partes que necesitan root, y se niega a ejecutarse en
una máquina que no sea el modelo en el que se verificó todo esto:

```bash
./scripts/install.sh
```

Vuelva a ejecutarlo después de cualquier cambio: todos los pasos son
idempotentes y reinicia el demonio en vez de solo habilitarlo, así que también
es el camino de actualización. Una bandeja que ya tenga abierta sigue ejecutando
la compilación anterior, y el script se lo dice.

Instalar el paquete por sí solo no arranca **nada**. Estos controles tienen un
único dueño, así que entregarlos es una decisión aparte y `dpkg -i` no debe
tomarla. Lo que hace el script, si prefiere hacerlo a mano:

```bash
sudo dpkg -i build-package/sombra_*.deb
sudo systemctl disable --now com.system76.PowerDaemon.service
sudo systemctl mask com.system76.PowerDaemon.service
sudo systemctl enable --now sombrad
sudo usermod -aG sombra "$USER"    # cierra la sesión y vuelve a entrar, para la bandeja
```

Que le añadan a un grupo con la sesión ya abierta no surte efecto hasta el
siguiente inicio de sesión, y esa es la razón habitual de que la bandeja diga
que el demonio no responde. El demonio entrega el socket a ese grupo él mismo,
justo después de vincularlo, y no desde el archivo de unidad, donde competiría
con su propia vinculación y fallaría en silencio.

`com.system76.PowerDaemon.service` es el nombre real de la unidad del
fabricante. `system76-power.service` es solo un `Alias=` suyo, y enmascarar el
alias deja habilitada la unidad real: volvería en el siguiente arranque y habría
dos dueños de una misma superficie de control. Enmascarar tampoco detiene una
unidad en marcha, y por eso se deshabilita primero.

`make install` sigue disponible para una compilación sin paquete. Instala los
mismos archivos, y las órdenes de arriba siguen siendo lo que pone al demonio al
mando.

## Uso

Las tres ranuras del panel del escritorio seleccionan los modos de este
proyecto. Todo lo demás pasa por el socket:

```bash
sombra status                   # lecturas y valores de consigna en JSON
sombra mode quiet
sombra set pl1=15 max_perf_pct=40   # en vivo, inmediato, todavía sin guardar
sombra mark too-loud            # un veredicto, marcado en la muestra
sombra fan max                  # solo por orden explícita
sombra export --format csv > session.csv
```

El ajuste en vivo no es duradero, y en eso está su razón de ser. Cuando un valor
resulta digno de conservarse, una sola orden lo escribe en un modo:

```bash
sombra config show                              # lo que está guardado, en JSON
sombra config set mode quiet pl1_w=10 pl2_w=16  # una definición duradera
sombra config save live to quiet                # conservar lo que encontraron los controles
sombra config default_mode work                 # el modo que se aplica al arrancar
sombra config reset modes                       # de vuelta a los valores de fábrica
```

### Dónde viven los ajustes

El demonio lleva compilados sus valores de fábrica y escribe lo que se le indica
en `/var/lib/sombra/settings.conf`. No hay ningún archivo en `/etc`: la edición
pasa por el demonio, para que un script que analice telemetría pueda aplicar lo
que ha encontrado sin root y sin editor de texto, y para que la bandeja y la
línea de órdenes no se separen. El archivo aparece la primera vez que se cambia
algo; bórrelo y vuelven los valores de fábrica.

Lo escribe el demonio, y los comentarios que haya dentro no sobreviven a un
cambio. Por eso el significado de cada clave está aquí abajo y no en el archivo.

| Clave | Unidad y rango | Significado |
|---|---|---|
| `label` | texto | lo que muestran el panel y la bandeja |
| `platform_profile` | `quiet`, `balanced`, `performance` | elige la curva del ventilador de la EC, que es el mando de ruido frente a temperatura |
| `pl1_w` | vatios, 1–45 | límite prolongado de RAPL; el control más eficaz bajo carga sostenida |
| `pl2_w` | vatios, `pl1_w`–65 | límite breve de RAPL |
| `max_perf_pct` | por ciento, 8–100 | techo de frecuencia de `intel_pstate` |
| `turbo` | `on`, `off` | |
| `epp` | `performance`, `balance_performance`, `balance_power`, `power` | preferencia de energía; junto con `hwp_dynamic_boost`, es lo que frena los picos cortos que calientan el paquete con la máquina nominalmente en reposo |
| `hwp_dynamic_boost` | `0`, `1` | |
| `gt_max_mhz` | MHz, 800–2350 | techo de iGPU. 800 es `gt_RPn_freq_mhz`, por debajo del cual el controlador rechaza la escritura sin más |
| `fan` | `auto`, `max` | `auto` deja al mando la curva que nombra `platform_profile`; `max` son unas 6100 RPM |

Los topes de `pl1_w` y `pl2_w` son los valores más altos confirmados en esta
máquina. Nada sube un límite por encima de ellos, y un número mayor se recorta y
se informa en vez de aceptarse. El kernel también ofrece `default` para `epp`, y
este demonio lo rechaza: el nodo lo acepta y luego informa de la preferencia que
eligió de verdad, así que al leerlo devuelve otra cosa y parece que otro proceso
esté escribiendo el control.

La velocidad máxima es lo bastante ruidosa como para que nada la elija en
silencio: un modo tiene que decir `fan=max`, o alguien tiene que escribir
`sombra fan max` o elegirla en la consola. La protección térmica de emergencia
es la excepción, y es un enclavamiento de seguridad, no un control automático.

Hay exactamente tres modos y se editan en su sitio, porque el panel del
escritorio tiene tres ranuras y una cuarta definición quedaría donde el panel no
puede llegar.

La consola habla el idioma del sistema, en ruso, inglés o español. Solo la
consola: `sombra --help`, las respuestas del demonio y las páginas de manual
siguen en inglés, porque se leen en un terminal, se citan en la documentación y
se buscan por su redacción exacta. Eso se nota en un sitio: una respuesta en la
línea de estado sale en inglés dentro de una ventana traducida. Es el precio
asumido a conciencia, no un descuido. Los catálogos están en
`/usr/share/sombra/translations`, y un idioma desconocido muestra el idioma de
origen, que es el inglés.

La pestaña *Ajustes* de la consola guarda lo que es del programa y no de la
máquina: si se inicia con la sesión, qué aspecto tiene, y un botón que devuelve
todas las definiciones de modo y todos los ajustes de retroiluminación a los
valores compilados en el demonio.

Iniciar con la sesión es una entrada de autoarranque XDG y nada más. Instalar el
paquete no añade ningún proceso a su sesión, y la entrada es todo el estado que
hay, así que lo que se apaga sigue apagado. Surte efecto en el próximo inicio de
sesión. La apariencia es clara, oscura o lo que diga el escritorio; es un ajuste
de la propia consola y no llega al icono del panel, que se dibuja con colores
fijos: la paleta es de la aplicación y el panel lo colorea el escritorio, y ambos
discrepan justo cuando el escritorio es claro con una barra oscura.

El icono es una nube sobre un sol y responde a tres cosas de un vistazo. La
posición de la nube dice qué modo está en marcha: en el margen más frío ocupa toda
la imagen, en el intermedio cubre la mitad del sol, en el más amplio no está.
Cálido frente a gris dice si el modo se conoce siquiera, y en el margen más frío,
donde no hay sol que colorear, ese color lo lleva la nube. Un demonio que no
responde y una respuesta sin modo dan la misma imagen gris: el icono habla del
modo, y ninguno de los dos casos sabe cuál es. La placa se pone roja mientras la
protección de emergencia limita la potencia. Las dos condiciones que la imagen no
puede llevar, un control sobrescrito por otro proceso y sensores que no se
pudieron leer, se nombran en la ayuda emergente junto con el modo, la temperatura
del paquete y las revoluciones del ventilador.

Hay una consola por sesión: si se arranca mientras otra está en marcha, le pide
a la otra que abra su ventana y termina. Dos consolas serían dos iconos en el
panel y dos sondeos al demonio, sin manera de saber qué icono es de cuál.

Arrancada con la sesión, la bandeja espera hasta un minuto a que aparezca un
panel antes de dar por hecho que no hay ninguno: el anfitrión de la bandeja se
registra por el bus de sesión y suele ir uno o dos segundos por detrás de un
proceso de autoarranque. Mientras no exista un icono, cerrar la consola termina
el proceso; si no, seguiría vivo sin ventana y sin icono, y solo se le podría
matar por el nombre.

## Brillo que sigue a la habitación

Una función aparte del modo térmico, con su propio interruptor. Elegir un modo
nunca mueve la retroiluminación, y nada de la retroiluminación llega al lado
térmico.

```bash
sombra brightness on
sombra brightness off             # se recuerda entre reinicios
sombra brightness level 80        # lo mismo que mover el control a mano
sombra brightness base 72         # el nivel alrededor del cual trabaja la automatización
sombra brightness set response_pct=35 floor_pct=20 ceiling_pct=90
sombra config set brightness response_pct=35 floor_pct=20   # queda escrito
```

El modelo en una línea: **la luz suma o resta al nivel que usted fija.**

- **El nivel que usted fija es la base.** Mover el brillo dice qué quiere con
  esta luz. El demonio le descuenta lo que la habitación aportaba en ese
  momento, y lo que queda es la base, y por eso volver a la misma luz devuelve
  exactamente el mismo brillo. La base también se puede fijar directamente, con
  `brightness base` o con el deslizador que está al lado del de brillo, que es a
  lo que hay que recurrir cuando la luz de la habitación no es la luz para la
  que se está ajustando.
- **La reacción dice cuánto puede moverlo la habitación**, en puntos
  porcentuales y lo mismo en cada sentido. Una reacción de 30 alrededor de una
  base del 80 % significa que una habitación oscura puede bajarlo al 50 % y una
  luminosa subirlo al 100 %.
- **El mínimo es el nivel por debajo del cual no baja nada**, ni la
  automatización ni un descuido de la mano. **El techo es el mismo límite por
  arriba**: mientras nadie lo mueva es la escala entera, y está para la pantalla
  que resulta molesta en su propio máximo.
- **No oscila.** El objetivo se redondea a escalones gruesos, cada frontera
  lleva un margen de histéresis, un escalón nuevo tiene que mantenerse varios
  segundos antes de que pase nada, y el movimiento en sí es una transición
  gradual y no un salto. Con luz estable no hay ninguna escritura.
- **Apagado es apagado.** Con la función apagada, el demonio no escribe la
  retroiluminación en absoluto, y encenderla o apagarla no mueve la
  retroiluminación por sí solo.

El nivel, la base, la reacción, el mínimo y el techo son cinco deslizadores en la
consola, todos en por ciento, porque «¿está bien así?» es un juicio que se hace
a ojo, en la habitación a la que va destinado. La lectura del sensor no está en
pantalla: lo que mide la habitación es asunto del demonio.

El único ajuste que no está en un control deslizante es la curva, que traduce
una lectura de luz en lux a una posición de 0 a 100: 0 resta toda la reacción,
50 no aporta nada, 100 suma toda la reacción. Su forma se calibra a partir de
telemetría grabada en vez de juzgarse en el momento, y 50 no es una elección
libre: le corresponde estar en la luz que la habitación tiene normalmente. Las
columnas `light_lux`, `brightness` y `brightness_base` de una sesión exportada
son con lo que hay que calibrarla.

## Qué sobrevive a un reinicio

| Se conserva | Por qué |
|---|---|
| el modo elegido | elegir uno es un acto deliberado |
| la automatización del brillo, encendida o apagada | algo que usted apagó no debe volver a encenderse por su cuenta |
| su brillo de base | si no, habría que fijarlo otra vez después de cada arranque |
| el umbral de carga | vive en sysfs, se pierde al reiniciar y nada más lo vuelve a aplicar |
| si la bandeja se inicia con la sesión | la entrada de autoarranque es el estado, así que lo apagado sigue apagado |
| los ajustes de los modos y de la retroiluminación | son lo que produce el ajuste, y perderlos significaría volver a ajustar |

Lo que a propósito no se conserva es el ajuste en vivo, que se aplica de
inmediato y se guarda solo cuando se pide. La velocidad máxima del ventilador
entra en eso: 6100 RPM sobreviviendo a un reinicio es una sorpresa, no un estado
restaurado.

Las definiciones y las elecciones de ejecución son dos archivos,
`settings.conf` y `state`, y están separados porque tienen vidas distintas: uno
se edita cuando se afinan los valores, el otro se reescribe cada vez que alguien
elige algo. La entrada de autoarranque es cosa de la bandeja y el demonio no
sabe nada de ella.

## Volver atrás

Si los modos de aquí resultan peores que los del fabricante, no hay que tocar
código:

```bash
./scripts/rollback.sh              # devuelve los controles
./scripts/rollback.sh --remove     # … y desinstala, conservando la telemetría
./scripts/rollback.sh --purge      # … y borra también la telemetría grabada
```

Devolver los controles es lo que ocurre por defecto, y deja en paz el paquete y
la telemetría grabada: la telemetría es el material con el que se afinan los
modos, y una mala tarde no es razón para tirarla. A mano:

```bash
sudo systemctl disable --now sombrad
sudo systemctl unmask com.system76.PowerDaemon.service
sudo systemctl enable --now com.system76.PowerDaemon.service
```

El demonio restaura el control automático del ventilador y un estado de potencia
confirmado como seguro en cualquier vía de salida, así que pararlo nunca deja la
máquina en un estado del que no pueda enfriarse sola. El script de vuelta atrás
comprueba que el ventilador volvió de verdad al control automático en vez de
darlo por hecho.

## Documentos

- `HARDWARE.md`: la superficie de control. Qué mandos de sysfs existen, qué
  hacen, los valores medidos y qué controles faltan en esta plataforma.
- `SCREENSHOTS.md`: qué aspecto tienen el menú de la bandeja y la consola, y la
  única orden que vuelve a hacer las imágenes.
- `DESIGN.md`: cómo está construido el proyecto y por qué. Las decisiones sobre
  su forma, con el razonamiento que las produjo.
- `AGENTS.md` (= `CLAUDE.md`): las convenciones del repositorio, el objetivo
  ante el que responde el proyecto y los controles de calidad.

Cada documento tiene al lado una copia en ruso (`<NAME>.ru.md`). El inglés es
canónico; donde los dos discrepen, el archivo en inglés es el correcto.

## Licencia

MIT, véase `LICENSE`.
