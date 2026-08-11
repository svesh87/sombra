*[English](DESIGN.md) · [Русский](DESIGN.ru.md) · Español*

# Cómo está hecho esto y por qué

Decisiones sobre la forma del proyecto, junto con el razonamiento que las
produjo. No están congeladas. Cada una está aquí para que cambiarla sea una
discusión con argumentos y no el descubrimiento de que alguien lo pensó una vez,
y para que una modificación en una se pueda contrastar con las demás.

Lo que está congelado es el objetivo, y está en `AGENTS.md`: un chasis frío
importa más que el rendimiento, el ruido tiene un techo calibrado y las reglas
de seguridad del hardware no se discuten. Todo lo que sigue sirve a ese objetivo
y responde ante él.

De ahí se siguen dos cosas. Una decisión de aquí se cambia con una edición
ordinaria, junto con el código, los documentos y sus traducciones. Y una
decisión que resultó equivocada se reescribe, no se anota al margen: este
archivo dice cómo es el proyecto, no cómo fue.

## Arquitectura

- **Los controles térmicos tienen exactamente un propietario: este demonio.**
  Dos procesos que escriben en los mismos nodos de sysfs dan un comportamiento
  que depende de quién gane la carrera, y no redundancia. El demonio del
  fabricante se enmascara con `systemctl mask com.system76.PowerDaemon.service`,
  por su nombre de unidad real y no por el alias `system76-power.service`:
  enmascarar un alias deja activada la unidad real, y vuelve en el siguiente
  arranque. Montar una capa encima de él y bifurcar su código quedan ambos
  descartados.
- **Instalar el paquete no toma posesión.** Imprime las dos órdenes que ceden los
  controles y ahí se detiene: no activa nada, no arranca nada, no enmascara nada.
  Ceder los controles térmicos es un acto deliberado con procedimiento de vuelta
  atrás, no un efecto secundario de `dpkg -i`, y el test de empaquetado lo
  comprueba. Para la consola tampoco se prepara nada: llega al demonio por el
  bus, y quién puede manejarlo es una pregunta que responde polkit.
- **La superficie de control es un solo nombre del bus del sistema,
  `io.github.svesh87.Sombra`.** Un socket de Unix es la alternativa evidente y
  queda descartada: sus permisos son un grupo más un modo de archivo, lo que
  cuesta un paso manual, `usermod -aG` y volver a iniciar sesión, y ese paso sería
  lo único que este proyecto le pide a una persona. Un archivo de política del bus
  y una acción de polkit dicen lo mismo sin que nadie entre en ningún sitio. Hay
  una sola superficie y no las dos, porque dos maneras de decir lo mismo son dos
  juegos de reglas sobre quién puede decirlo.
- **Todo lo que cambia el estado se comprueba contra una sola acción de polkit,
  con la interacción desactivada.** `allow_active=yes`; `allow_inactive` y
  `allow_any`, no: a quien está delante de esta pantalla se le responde en
  silencio, y una sesión remota o inactiva se rechaza. Nunca se pide una
  contraseña. Un `auth_admin` delante de elegir una curva de ventilador más
  silenciosa sería teatro, porque quien ya está en el teclado llega a los mismos
  controles con un editor de texto, y un demonio que saca un diálogo cada vez que
  se mueve un deslizador es un demonio que nadie usa. Una acción y no varias,
  porque entre estas órdenes no hay ninguna diferencia que alguien quisiera
  responder de otra manera.
- **Las lecturas y los ajustes son propiedades; lo que cambia el estado es un
  método.** Un cliente lo lee todo una vez con `GetAll` y después se le avisa de
  lo que cambia, así que la consola no sondea nada. La separación importa: lo que
  se mueve en cada vuelta del bucle viaja en un solo diccionario, `Readings`, y
  todo lo que cambia porque alguien lo ha cambiado es una propiedad aparte, de
  manera que una profundidad de retención nueva no llega dentro de unas
  revoluciones del ventilador. `Readings` es una propiedad y no una señal porque
  una propiedad se puede pedir: `sombra status` no espera a la vuelta siguiente,
  y `busctl get-property` muestra el estado de la máquina sin que nadie escriba
  un cliente.
- **El demonio funciona sin bus y toma su nombre cuando aparece uno.** Es dueño
  del margen térmico: un bus inalcanzable cuesta una consola, no la
  refrigeración. Lo único por lo que se niega a arrancar es que otro demonio ya
  tenga el nombre: esta arquitectura existe para evitar que una superficie de
  control tenga dos dueños.
- **Un solo vocabulario, dos clientes y un demonio que lo analiza una vez.** Las
  palabras que acepta `sombra` y las que la consola muestra en su línea de estado
  son las mismas, y en un único sitio se convierten en una llamada con tipos; el
  demonio convierte la llamada de vuelta en una de sus propias líneas de órdenes,
  porque ese analizador es donde un valor se valida, se recorta o se rechaza. Una
  vía con tipos que llegara directa al estado sería un segundo juego de reglas
  sobre qué valores se admiten. Las dos direcciones tienen su inversa y los tests
  las comparan: una correspondencia escrita dos veces solo es de fiar si las dos
  mitades se contrastan entre sí.
- **La definición del protocolo está en un solo archivo XML, y un test obliga al
  demonio a cumplirla.** El adaptador está escrito a mano, porque un método que
  espera a polkit tiene que responder más tarde de lo que retorna, y un adaptador
  generado no llega al mensaje que está respondiendo. Así que un caso de prueba
  lee la introspección de un demonio en marcha y la compara con
  `dbus/io.github.svesh87.Sombra.xml`: «descrito en un solo sitio» pasa así a ser
  un hecho y no una promesa.
- Por compatibilidad, el demonio responde además en `net.hadess.PowerProfiles` y
  `org.freedesktop.UPower.PowerProfiles` (la interfaz pública estándar) y en
  `com.system76.PowerDaemon` (propia del fabricante, para el applet del panel de
  COSMIC). Esa es otra superficie, con otro propósito y en una conexión propia:
  la política de D-Bus de serie ya permite que un proceso de root sea propietario
  de esos tres nombres, y no dicen nada de la retroiluminación, de los monitores
  ni de la telemetría.
- **El brillo de la pantalla nunca está ligado al modo térmico.** Cambiar la
  retroiluminación al cambiar de perfil es un comportamiento de `system76-power`
  que este proyecto considera un defecto, y recuperar la paridad con el original
  en ese punto no es una mejora. Elegir un modo tiene que dejar la
  retroiluminación exactamente donde estaba.
- **El brillo que sigue al sensor de luz ambiental es una función aparte, con su
  propio interruptor.** Reacciona a la habitación y no al perfil de energía, y
  por eso no contradice la regla anterior. Nada del modo térmico llega hasta
  esta función, en ninguna de las dos direcciones.
- **La luz suma o resta al nivel que fijó la persona.** Una corrección a mano es
  una indicación y no una interrupción: al mover el brillo, la persona dice qué
  quiere con esta luz, y el demonio le resta la aportación de la habitación en
  ese momento para obtener la base. Eso es lo que hace que la misma luz devuelva
  el mismo nivel. Un número fija cuánto puede moverlo la habitación en cada
  sentido, y un mínimo y un techo fijan los niveles por debajo y por encima de los
  cuales no anda nadie. Ambos obligan también a la persona: una pantalla llevada a
  ilegible, o a molesta, es un fallo y no un ahorro. El techo es la escala entera
  mientras nadie lo mueva, y existe porque una pantalla puede ser demasiado
  brillante para sentarse delante, y porque los paneles externos hacen distinta
  cantidad de luz con el mismo tanto por ciento.
- **Los monitores externos son del demonio, y su bus vive en un hilo.** El brillo
  de un monitor es DDC/CI por el bus I2C de su conector, no un nodo de sysfs: medido
  en esta máquina, una lectura cuesta unos 350 ms y una escritura unos 300 ms, y dos
  monitores son dos buses en secuencia. En el bucle de 1 Hz está la protección de
  emergencia y no espera a una pantalla, así que el bus se trabaja en un hilo propio
  y las respuestas llegan como señales. Es del demonio y no de la consola por lo
  mismo que el panel: la automatización que seguirá a la luz vivirá aquí, y la
  consola pide.
- **Un gesto se colapsa en vez de encolarse, y la consola no pelea con la mano.** El
  bus no puede servir a un deslizador: veinte valores llegan más rápido de lo que
  termina una escritura. Así que el demonio guarda un valor pendiente por monitor y lo
  reemplaza, y al bus va el último: un arrastre cuesta una escritura o dos. Del lado
  de la consola, un control con una mano encima no se repinta con una respuesta de
  estado, y tampoco el que espera la confirmación del valor que acaba de enviar: la
  respuesta en mano tiene medio segundo, y repintar con ella es lo que arranca el
  deslizador de debajo de los dedos.
- **Una escritura a un monitor la confirma una lectura, nunca su propia respuesta.**
  Uno de los paneles medidos aquí informa de un fallo y aplica el valor de todas
  formas. Así que una escritura significa «enviada», decide la lectura siguiente, y
  la verificación propia de la biblioteca —que lee demasiado pronto para ese panel—
  está apagada.
- **Una ranura de una muestra es un conector, no un orden de llegada.** Los
  conectores externos ordenados por nombre, tres en esta máquina, y un conector que
  invente un concentrador DP-MST no recibe ranura. Una ranura que significara
  pantallas distintas en muestras distintas haría ilegible una sesión. Un byte por
  ranura dice qué monitor hay en ella: por un mismo conector de esta mesa pasaron
  tres paneles en una hora.
- **La base se puede fijar directamente, y no solo deducirse.** Al mover el
  brillo, la persona dice qué quiere con la luz del momento, y la base sale de
  ahí; esa es la dirección natural y falla en una sola cosa: ajustar para una
  luz que la habitación no tiene ahora mismo. Por eso la base tiene su propia
  orden y su propio control, y entonces el nivel sale de ella. La base vive en
  el archivo de estado y no en los ajustes: es lo último que la persona estuvo
  mirando, no un número que configuró.
- **No hay perfiles de brillo.** Una habitación, un juego de ajustes: una
  reacción, un mínimo, un techo y una curva. Un perfil sería un nombre para un juego de
  esos números, y nunca hubo un segundo juego al que cambiar.
- **La curva se calibra con la telemetría, no se elige a ojo.** Traduce los lux
  a una posición de 0 a 100, y la posición en la que no aporta nada corresponde
  a la luz que la habitación tiene normalmente. De ella no se saca nada a un
  deslizador.
- **Una retroiluminación que se ve oscilar es un defecto, no una cuestión de
  ajuste.** A la automatización la mira quien se sirve de ella, y la mira todo
  el día: un solo paso en falso cuesta más atención de la que ahorra toda la
  función. Se mueve a pasos gruesos, con histéresis en sus límites, espera antes
  de actuar sobre un cambio y hace una transición en lugar de saltar. Con luz
  estable no debe haber escritura alguna.
- **La automatización viene apagada, se apaga con una sola acción y sigue
  apagada tras un reinicio.** Lo que la persona apagó no debe volver a
  encenderse por su cuenta. Encenderla o apagarla no mueve la retroiluminación:
  un salto de brillo en respuesta a un interruptor es el mismo defecto que un
  salto en respuesta a un cambio de perfil.
- Las tres ranuras del panel se redefinen como los modos de este proyecto. **El
  modo del medio es el de trabajo: la máquina pasa en él la inmensa mayoría del
  tiempo.** Se aplica al arrancar y es el que se ajusta con la telemetría. Los
  otros dos son casos extremos.
- **El ventilador es un valor de consigna como los demás.** Un modo nombra una
  de las tres curvas del firmware y puede pedir la velocidad máxima
  (`pwm1_enable=0`), y la consola ofrece las cuatro en una sola lista, para que
  «ahora hace demasiado calor» sea un clic. Lo prohibido es otra cosa: que la
  máquina llegue ahí sin que nadie se lo pida, con la protección de emergencia
  como única excepción.
- Funciones que se heredan junto con la propiedad y por eso son obligatorias:
  reaccionar al paso entre red y batería, y el umbral de carga en
  `/sys/class/power_supply/BAT0/charge_control_end_threshold`.

## Sin regulación automática

El demonio aplica **valores de consigna estáticos por modo**. No mantiene un
lazo de realimentación que ajuste el límite de potencia de forma continua, y no
cambia de modo por su cuenta.

- **No hay regulador con realimentación.** Lo medido hasta ahora indica que los
  valores de consigna estáticos bastan tanto en reposo como, por lo que se ve,
  bajo carga sostenida. Construir un regulador solo si una medición demuestra
  que los valores estáticos son a la vez demasiado restrictivos y demasiado
  ruidosos en alguna situación real. Hasta entonces, un regulador es maquinaria
  injustificada, con coeficientes que hay que ajustar y con modos de fallo, la
  oscilación y el atasco, que los valores estáticos no pueden tener.
- **No hay cambio automático de modo según el contexto.** Elegir un modo desde
  el panel o desde la bandeja es un acto deliberado. Cambiar según la actividad
  del micrófono o de la cámara es una idea razonable, y queda aplazada hasta que
  el uso manual dé datos.
- **Esto no anula la protección térmica de emergencia.** Un tope duro que
  recorta la potencia cuando el paquete se acerca al corte por hardware es un
  seguro y no regulación, y está activo siempre, sea cual sea el modo. Con el
  historial de apagados térmicos por hardware que tiene esta plataforma,
  quitarlo no es nunca una simplificación.
- **Reproducir el comportamiento de serie del fabricante no es un modo.** Ese
  comportamiento es precisamente el problema que este proyecto viene a arreglar.
  La salida de emergencia, si los modos de aquí resultan peores, es
  `systemctl unmask com.system76.PowerDaemon.service`, sin necesidad de código.

## Ajustes

- **Los ajustes pertenecen al demonio.** Los valores por omisión van compilados
  en el código; lo que se ha cambiado vive en un archivo de texto que escribe el
  demonio, `/var/lib/sombra/settings.conf`, y no se instala ningún archivo en
  `/etc`. La razón no es la comodidad: los parámetros de los modos se afinan con
  un análisis automático de la telemetría grabada, y quien hace ese análisis
  tiene que poder aplicar lo que encontró, sin root y sin manejar un diálogo.
- **La edición pasa por el demonio y por ningún otro sitio.** `sombra config` y
  la bandeja envían las mismas órdenes, los valores los valida el mismo código
  que lee el archivo, y hay exactamente un escritor. El archivo lo escribe la
  máquina: los comentarios puestos a mano no sobreviven a un cambio, y por eso
  el vocabulario del archivo está documentado en `../README.md`. El modo elegido y
  las demás decisiones de ejecución quedan en un archivo de estado aparte,
  porque tienen otro ciclo de vida. Volver a leer un archivo modificado desde
  fuera sigue haciéndose por inotify, sin reiniciar.
- **Los modos son exactamente tres y se editan en su sitio.** El panel del
  escritorio tiene tres ranuras; una cuarta definición existiría solo allí donde
  el panel no llega. Crear y borrar modos no es, por tanto, una función.
- **Un restablecimiento devuelve los valores incorporados**, por grupos y cuando
  se pide. Es la salida de un enredo y el estado de una instalación nueva, no
  una operación de todos los días.
- **Lo aplicado y lo escrito son dos propiedades de una misma cosa.** Mover un
  control lo aplica al instante y no escribe nada, y el selector de modo se
  queda vacío, porque lo que está funcionando ya no es uno de los tres. Guardar
  es un acto aparte que nombra el modo donde caen los números. Por eso es una
  sola pestaña y no dos: una distribución que separa «se aplica ahora» de «está
  escrito» muestra los mismos números dos veces y obliga a la persona a llevar
  esa diferencia en la cabeza.
- **La bandeja nunca escribe el archivo de ajustes por su cuenta.** Se lo pide
  al demonio, con las mismas órdenes que envía la línea de órdenes. Dos
  escritores de un archivo es el mismo error que dos propietarios de una
  superficie de control.
- **Lo que el demonio tiene guardado se publica, no se pide.** Las definiciones
  son una propiedad como todo lo demás, así que un modo editado desde la línea de
  órdenes llega a una consola abierta sin que ella tenga que volver a preguntar.
- **El icono del panel se dibuja, no se pide por su nombre.** Publicar un nombre
  estándar del tema dejaría que el panel lo dibujara y le cambiase el color, y
  no funciona: un host de StatusNotifierItem prefiere el nombre a la imagen que
  va con él y dibuja un sustituto cuando su propio tema no resuelve ese nombre,
  mientras que este proceso solo puede preguntar a su tema de iconos, que sigue
  otra herencia y responde que sí donde el panel responde que no. Tampoco hay
  ningún glifo que tomar prestado: los temas de aquí traen tríos sobre volumen,
  brillo, micrófono y seguridad, y ninguno sobre energía. Los colores están
  fijados y no se toman de la paleta, porque la paleta es de la aplicación y el
  panel es del escritorio.
- **El icono del panel lleva su propia placa.** Es la imagen del icono de la
  aplicación, una nube sobre un sol, y un sol cálido no puede ser un glifo
  plano: contra un panel claro se queda en 1.5:1 cuando el mínimo es 3.5, y el
  único tono uniforme que lo supera en los dos paneles es un ocre oscuro que ya no
  se lee como un sol. Con una placa, el sol se ve contra ella, y lo único que se
  compara con el escritorio es la placa misma, que supera 3.5:1 en ambos casos. La
  placa es lisa donde la del icono de la aplicación es un degradado, cuyo extremo
  oscuro mide 1.3:1 contra una barra oscura y se disuelve en ella.
- **Tres canales, y la protección de emergencia es la placa.** La posición de la
  nube dice qué modo está en marcha, cálido frente a gris dice si el modo se conoce
  siquiera, y la placa se pone roja bajo la protección de emergencia. Si en su
  lugar se pusiera rojo el sol, lo separaría de la placa azul solo el tono, a
  1.2:1, la única señal que no sobrevive ni a un tamaño pequeño ni a un ojo
  daltónico. En el margen más frío el sol no se dibuja: unos rayos saliendo
  alrededor de la nube serían un modo con la luz todavía encendida, así que allí el
  segundo canal es el color de la nube misma. Un demonio que no responde y una
  respuesta sin modo son una sola imagen, porque el icono habla del modo y ninguno
  de los dos casos sabe cuál es; cuál de ellos ha ocurrido lo dice la ayuda
  emergente.
- **La nube se separa del sol con una franja del color de la placa.** A 22 píxeles
  el borde común entre una nube clara y un sol cálido es un único píxel mezclado, y
  en lugar de dos objetos queda uno. Son 11 unidades de la retícula de 256: por
  debajo de siete la franja es más fina que medio píxel a ese tamaño y desaparece
  en el suavizado, por encima de catorce se lee como un contorno dibujado alrededor
  de una pegatina.
- **Una condición que la imagen no puede llevar va en la ayuda emergente.** Tanto
  un control sobrescrito por otro proceso como sensores que no se pudieron leer
  llegan desde el demonio, y la ventana los muestra como avisos, pero primero hay
  que abrir la ventana, y conviene saber de ellos sin abrir nada. No hay un cuarto
  canal que sobreviva a 22 píxeles, y un segundo color de alarma se confundiría con
  la protección de emergencia, que es el único estado que cambia lo que hace la
  máquina sin que nadie lo pida. Así que se nombran con palabras en la ayuda
  emergente, donde hay sitio.
- **La consola se traduce; la línea de órdenes no.** La ventana sigue el idioma
  del sistema, con los catálogos instalados junto a los binarios. La línea de
  órdenes, las respuestas del demonio y las páginas de manual siguen en inglés:
  se leen en un terminal, se citan en documentos y se buscan por su formulación
  exacta, y una línea `err` traducida no la encuentra nadie. El precio se ve en
  un solo sitio, una respuesta en inglés dentro de una ventana traducida, y se
  asume en lugar de esquivarlo.
- **La explicación va en el control que explica.** Los párrafos en la ventana
  quitaban a los controles la altura que necesitaban y duplicaban el texto a
  traducir, así que lo que hace cada control se dice en su ayuda emergente. Un
  test recorre todos los controles que envían una orden y falla si alguno no
  tiene nada que decir.
- **Una ayuda emergente no puede ser una referencia, y de ahí la página fuera de
  la ventana.** Una ayuda emergente explica un control a quien ya sabe sobre qué
  pasar el ratón. No responde qué es este programa, y no se puede leer al lado de
  otra igual, y comparar dos controles es la mayor parte de para qué sirve una
  referencia. Así que la página documenta cada control de cada pestaña, con sus
  unidades, su rango y lo que cuesta; hay un `?` al final de la línea de estado y una
  página instalada por idioma, que abre lo que el escritorio use para abrir
  documentos. Se abre en la sección de la pestaña desde la que se pulsó: preguntado
  desde la pestaña de la retroiluminación, «qué hace esto» significa la
  retroiluminación. Los anclajes son los nombres internos de las pestañas, los mismos
  con los que se nombran las imágenes, así que un solo juego sirve para todos los
  idiomas. Dentro de la ventana serían los párrafos que
  se sacaron de ella, y no hay sitio: la altura es la de la pestaña más grande. Que
  falte la página no es un fallo — el botón lo dice en lugar de no hacer nada.
- **Una sola línea para el estado del demonio y para sus respuestas.** Dos
  suponían una franja de vacío bajo cada pestaña hasta la primera orden. Una
  respuesta aceptada devuelve la línea a los cinco segundos; una negativa la
  retiene hasta la orden siguiente, porque responde a algo que hizo la persona y
  unos pocos segundos no bastan para dar por seguro que se ha leído.
- **Los colores de la consola vienen del escritorio y lo siguen mientras la
  ventana está abierta.** Un escritorio que se ocupa de las aplicaciones Qt les
  escribe un esquema de color; la consola lee ese archivo y lo vuelve a leer
  cada vez que el portal de apariencia avisa de que algo se ha movido. Esto no
  es un lujo innecesario: el escritorio cambia de tema por su cuenta, al
  amanecer y al atardecer; un tema de plataforma no actualiza la paleta de una
  aplicación en marcha, y volver a abrir la ventana no reinicia el proceso. Sin
  volver a leer el archivo, una consola que lleva todo el día abierta se queda
  con los colores de ayer. Donde no hay esquema escrito, la respuesta del portal
  elige entre dos paletas propias, y la pestaña de ajustes puede anular esa
  elección por completo.
- **La barra de título se deja en paz.** En este escritorio es la de Qt, porque el
  compositor ofrece dibujarla y Qt 6.4 no se lo pide, y toma sus colores cuando se
  crea la ventana. Un cambio de esquema llega por tanto a los widgets al instante y
  a la barra en la siguiente apertura. Reconstruir la ventana para refrescar la
  barra se probó y se llevó el proceso por delante; cerrar una vez y hacer un clic
  en el icono del panel sale más barato.
- **Lo que no se intenta es imitar los widgets del escritorio**, sus esquinas
  redondeadas ni el tinte de sus contenedores. Eso es una hoja de estilos
  persiguiendo un blanco en movimiento, y lo que desentona en una ventana son
  precisamente los colores.
- **La única excepción es la propia entrada de arranque automático de la
  bandeja, y no es configuración.** Que la bandeja arranque con la sesión es
  algo que la consola sabe de sí misma; el archivo del demonio no tiene forma de
  describirlo, y desde esa casilla no se llega ni a la definición de un modo ni
  a los ajustes de la retroiluminación. La bandeja es propietaria de
  `$XDG_CONFIG_HOME/autostart/sombra-tray.desktop` y de nada más. La entrada es
  el estado, detrás de la casilla no hay archivo de ajustes, y eso es lo que
  hace que «lo apagué y siguió apagado» sea una propiedad de cómo está montado y
  no de un mecanismo. El paquete no escribe nada en el arranque automático:
  `dpkg -i` no debe añadir un proceso a una sesión, por la misma razón por la
  que no se queda con los controles térmicos. La elección de apariencia está al
  lado, en el archivo de ajustes propio de la consola, y por la misma razón: es
  cosa de la ventana y no de la máquina, y el demonio no tiene nada que hacer
  guardándola.
- **Cada valor de consigna en vigor se escribe en cada muestra de telemetría**:
  el modo activo, los límites de potencia, el techo de frecuencia, el turbo,
  EPP, HWP dynamic boost y el perfil de plataforma. Un registro que no dice qué
  se aplicó no se puede analizar. Los cambios de modo y de valores de consigna
  se deducen comparando muestras consecutivas; no añadir para eso un registro de
  eventos aparte.
- **Los veredictos subjetivos también son datos.** La bandeja puede marcar la
  muestra actual con una valoración («bien», «demasiado ruidoso», «demasiado
  lento»), para que un juicio hecho de oído se recupere después junto con los
  ajustes que lo provocaron. Sin esto, el ajuste en vivo produce una opinión que
  no se puede atar a números.

## Almacenamiento de métricas

- **En memoria, con volcados poco frecuentes al disco.** Cuidar la vida útil del
  NVMe es deliberado; las propuestas de escribir cada muestra directamente en el
  disco se rechazan.
- Los límites de retención son estrictos: el almacén no crece, y el desalojo es
  una propiedad del formato y no de una tarea de limpieza en segundo plano.
- El formato se elige de modo que con él se puedan afinar los parámetros de los
  modos y dibujar gráficas. Un promediado que esconda los picos es inadmisible.
- **Un cambio en la disposición del registro convierte lo ya grabado, no lo
  tira.** En las muestras hay una semana de vida de la máquina y varias tardes
  de ajuste, y la disposición cambia por motivos que no tienen nada que ver con
  ellas. La conversión es una cadena de pasos que suben el formato una versión
  cada uno; cada paso se escribe una vez y luego se congela, así que una versión
  nueva cuesta una estructura y una función. Los campos que la disposición
  antigua no tenía se graban como ausentes, nunca como ceros.
- **La profundidad del anillo, y si se escribe en él, son ajustes.** La profundidad
  estaba compilada, y no es una política aplicada al anillo sino su propio tamaño:
  cambiarla reconstruye el archivo y arrastra las muestras, y acortarla descarta las
  más antiguas, que es en lo que consiste acortarla. De un día a treinta, siete por
  omisión; hay techo porque el archivo ocupa todo su tamaño desde que se crea y quien
  mueve un contador no ha aceptado un gigabyte. Apagar la grabación detiene solo el
  añadido: la muestra se sigue tomando, `status` sigue respondiendo y el archivo se
  queda como está. Un interruptor que borrase una semana de medidas sería un
  interruptor que nadie se atreve a tocar.
- **El registro son 128 bytes, la mitad de ellos en reserva, y lo arriesgado aquí
  es cambiar ese tamaño.** Un campo nuevo exige una versión de todos modos: en su
  sitio, un registro antiguo tiene ceros y lo que hace falta es ausencia. La
  reserva evita otra cosa, que el tamaño vuelva a cambiar, y es el tamaño lo que
  convierte «¿esto es un anillo?» en una pregunta sobre la versión de la cabecera y
  no sobre el registro con el que se compiló esta versión. Responderla con
  `sizeof(Sample)` funciona exactamente mientras todas las versiones midan lo
  mismo, y el día que una no lo haga aparta una semana de muestras.
- **Un anillo que esta compilación no sabe leer se guarda, no se borra, y nunca
  detiene al demonio.** Una disposición de una compilación más nueva tras una
  vuelta atrás, una que nunca se publicó, una cabecera estropeada por un corte
  de luz: todas se apartan con sus bytes intactos y en su lugar entra un anillo
  limpio. La telemetría es secundaria frente a mantener la máquina dentro de su
  margen térmico, así que un anillo ilegible cuesta las muestras que hay en él y
  nada más.
- **Un solo espacio de copia de seguridad, sobrescrito cada vez.** Si no, bajar
  de una versión a la siguiente, y luego otra vez, dejaría un anillo en cada
  paso, en un disco cuya vida útil es la razón de ser del resto de esta sección,
  y el del medio no le sirve a nadie. La disposición va en la cabecera,
  así que el nombre no tiene por qué llevarla.
