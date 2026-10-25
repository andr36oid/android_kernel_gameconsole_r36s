#!/usr/bin/env python3
"""Compare compiled GRC802 panel and input wiring with the stock DTB."""
import subprocess
import sys

stock, built = sys.argv[1:]
panel = '/dsi@ff450000/panel@0'

def get(blob, node, prop, kind='x'):
    return subprocess.check_output(['fdtget', '-t', kind, blob, node, prop], text=True).strip()

def cells(blob, node, prop):
    return [int(v, 16) for v in get(blob, node, prop).split()]

def gpio(blob, values):
    banks = ['/pinctrl/gpio%d@%s' % (i, addr) for i, addr in
             enumerate(['ff040000', 'ff250000', 'ff260000', 'ff270000'])]
    return [(banks.index(next(p for p in banks if cells(blob, p, 'phandle') == [values[i]])),
             values[i+1], values[i+2]) for i in range(0, len(values), 3)]

for prop in ['panel-init-sequence', 'panel-exit-sequence']:
    a = get(stock, panel, prop, 'bx').split()
    b = get(built, panel, prop, 'bx').split()
    assert a == b, prop
    print('%s: %d identical bytes' % (prop, len(a)))
assert len(get(built, panel, 'panel-init-sequence', 'bx').split()) == 798
for prop in ['compatible', 'prepare-delay-ms', 'reset-delay-ms', 'init-delay-ms',
             'enable-delay-ms', 'disable-delay-ms', 'unprepare-delay-ms',
             'width-mm', 'height-mm', 'dsi,flags', 'dsi,format', 'dsi,lanes']:
    assert get(stock, panel, prop, 's' if prop == 'compatible' else 'x') == get(built, panel, prop, 's' if prop == 'compatible' else 'x'), prop
for prop in ['clock-frequency', 'hactive', 'vactive', 'hfront-porch', 'hsync-len',
             'hback-porch', 'vfront-porch', 'vsync-len', 'vback-porch',
             'hsync-active', 'vsync-active', 'de-active', 'pixelclk-active']:
    path = panel + '/display-timings/timing0'
    assert get(stock, path, prop) == get(built, path, prop), prop
assert gpio(stock, cells(stock, panel, 'reset-gpios')) == gpio(built, cells(built, panel, 'reset-gpios'))
keys = ['r1', 'r2', 'l1', 'l2', 'a', 'b', 'x', 'y', 'left', 'right', 'up', 'down',
        'select', 'start', 'fn', 'mode', 'l3', 'r3']
expected = gpio(stock, cells(stock, '/gamepad', 'key-gpios'))
actual = [gpio(built, cells(built, '/gamepad-keys/' + k, 'gpios'))[0] for k in keys]
assert expected == actual
for name in ['rocker0', 'rocker', 'rocker1']:
    assert gpio(stock, cells(stock, '/gamepad', name + '-gpios')) == gpio(built, cells(built, '/gamepad-keys/' + name, 'gpios'))
for name, channels in [('left-stick', [0, 1]), ('right-stick', [2, 3])]:
    assert cells(built, '/' + name, 'io-channels')[1::2] == channels
assert get(built, '/firmware/android', 'lcd_density', 's') == '304'
print('Stock timings, panel properties, reset, 18 keys, 3 rockers, 4 ADC axes and density verified')
# Radio checks run only on the Phase 2 tree. Phase 1 intentionally disables SDIO.
if get(built, '/dwmmc@ff380000', 'status', 's') == 'okay':
    sdio = '/dwmmc@ff380000'
    assert cells(built, sdio, 'bus-width') == cells(stock, sdio, 'bus-width')
    assert cells(built, sdio, 'max-frequency') == cells(stock, sdio, 'max-frequency')
    assert gpio(stock, cells(stock, '/wireless-wlan', 'WIFI,poweren_gpio')) == [(0, 2, 0)]
    assert gpio(built, cells(built, '/sdio-pwrseq', 'reset-gpios')) == [(0, 2, 1)]
    irq = cells(built, sdio + '/wifi@1', 'interrupts')
    assert irq == [gpio(stock, cells(stock, '/wireless-wlan', 'WIFI,host_wake_irq'))[0][1], 4]
    for prop in ['BT,reset_gpio', 'BT,wake_gpio', 'BT,wake_host_irq', 'uart_rts_gpios']:
        assert gpio(stock, cells(stock, '/wireless-bluetooth', prop)) == gpio(built, cells(built, '/wireless-bluetooth', prop)), prop
    assert get(built, '/wireless-bluetooth', 'status', 's') == 'disabled'
    assert get(built, '/serial@ff158000', 'status', 's') == get(stock, '/serial@ff158000', 'status', 's')
    print('Stock SDIO controller, power/wake pins, BT pins and UART1 status verified')
