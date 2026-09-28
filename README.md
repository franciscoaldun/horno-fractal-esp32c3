# Banco de pruebas térmico — ESP32-C3

Firmware para **ESP32-C3** que **exprime el núcleo al 100%** renderizando un fractal de **Mandelbrot con zoom infinito** (en aritmética de punto fijo de 64 bits) y muestra en vivo la **temperatura interna del chip** en una OLED. Pensado como banco de pruebas para **medir disipadores**: cargas el CPU al máximo y ves cuántos grados baja cada disipador.

Escrito en C sobre **ESP-IDF v6.1**.

![target](https://img.shields.io/badge/target-ESP32--C3-red) ![framework](https://img.shields.io/badge/framework-ESP--IDF%20v6.1-blue) ![estado](https://img.shields.io/badge/estado-probado%20en%20hardware-brightgreen) ![lenguaje](https://img.shields.io/badge/lenguaje-C-555)

## Características
- **Carga de CPU al 100%** a 160 MHz con compilación en `-O2` (fractal en punto fijo, sin FPU).
- **Sensor de temperatura interno** del ESP32-C3 leído en tiempo real (actual y máxima).
- Fractal animado + barra de estado (temperatura, FPS) en OLED 128×32.
- Alimenta el watchdog correctamente pese a la carga máxima.

## Por qué es útil
El ESP32-C3 no trae FPU, así que un Mandelbrot es puro trabajo entero: mantiene el único núcleo saturado de forma estable y repetible. Ideal para comparar disipadores: anota la temperatura máxima sin disipador y con él.

## Hardware
| Componente | Detalle |
|---|---|
| MCU | ESP32-C3 SuperMini |
| Pantalla | OLED SSD1306 128×32 (I2C, dir. 0x3C) |
| Conexión | `SDA → GPIO8`, `SCL → GPIO9`, `VCC → 3V3`, `GND → GND` |

## Compilar y flashear
```bash
idf.py set-target esp32c3
idf.py build
idf.py -p COMx flash monitor
```

## Detalle técnico
El sensor de temperatura del C3 exige un **rango predefinido** que contenga el intervalo pedido; se usa `TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100)`. Un rango que no calce en uno predefinido aborta el arranque.

## Probado en hardware
Funcionando en un **ESP32-C3 SuperMini** con OLED SSD1306 de 0,91":
- el núcleo queda al **100 %** de forma sostenida y el watchdog no salta;
- la barra muestra la temperatura interna actual y la máxima, más los FPS del fractal;
- el sensor arranca con el rango `20–100 °C` (el `-10–110` aborta: está documentado arriba).

## Autor
Desarrollado por **Francisco Aldunate** — firmware para ESP32 (P4, S3 y C3) en C con ESP-IDF, el framework oficial de Espressif.
Portafolio: [franciscoaldunate.cl](https://franciscoaldunate.cl) · GitHub: [@franciscoaldun](https://github.com/franciscoaldun)

## Licencia
MIT — ver [LICENSE](LICENSE).
