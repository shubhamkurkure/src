/*	$OpenBSD: sxipio.c,v 1.1 2017/01/21 08:26:49 patrick Exp $	*/
/*
 * Copyright (c) 2010 Miodrag Vallat.
 * Copyright (c) 2013 Artturi Alm
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/gpio.h>
#include <sys/evcount.h>
#include <sys/malloc.h>

#include <machine/bus.h>
#include <machine/fdt.h>
#include <machine/intr.h>

#include <dev/gpio/gpiovar.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_clock.h>
#include <dev/ofw/ofw_gpio.h>
#include <dev/ofw/ofw_pinctrl.h>
#include <dev/ofw/fdt.h>

#include <dev/fdt/sunxireg.h>
#include <dev/fdt/sxipiovar.h>

#include "gpio.h"

#define	SXIPIO_NPORT		9

struct sxipio_softc;

struct sxipio_gpio {
	struct sxipio_softc *sc;
	int port;
};

struct intrhand {
	int (*ih_func)(void *);		/* handler */
	void *ih_arg;			/* arg for handler */
	int ih_ipl;			/* IPL_* */
	int ih_irq;			/* IRQ number */
	int ih_gpio;			/* gpio pin */
	struct evcount ih_count;
	char *ih_name;
};

struct sxipio_softc {
	struct device		sc_dev;
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	void			*sc_ih_h;
	void			*sc_ih_l;
	int 			sc_max_il;
	int 			sc_min_il;

	struct sxipio_pin	*sc_pins;
	int			sc_npins;
	struct gpio_controller	sc_gc;

	struct sxipio_gpio	sc_gpio[SXIPIO_NPORT];
	struct gpio_chipset_tag	sc_gpio_tag[SXIPIO_NPORT];
	gpio_pin_t		sc_gpio_pins[SXIPIO_NPORT][32];

	struct intrhand		*sc_handlers[32];
};

#define	SXIPIO_CFG(port, pin)	0x00 + ((port) * 0x24) + (((pin) >> 3) * 0x04)
#define	SXIPIO_DAT(port)	0x10 + ((port) * 0x24)
#define	SXIPIO_DRV(port, pin)	0x14 + ((port) * 0x24) + (((pin) >> 4) * 0x04)
#define	SXIPIO_PUL(port, pin)	0x1c + ((port) * 0x24) + (((pin) >> 4) * 0x04)
#define	SXIPIO_INT_CFG0(port)	0x0200 + ((port) * 0x04)
#define	SXIPIO_INT_CTL		0x0210
#define	SXIPIO_INT_STA		0x0214
#define	SXIPIO_INT_DEB		0x0218 /* debounce register */

#define SXIPIO_GPIO_IN		0
#define SXIPIO_GPIO_OUT		1

int	sxipio_match(struct device *, void *, void *);
void	sxipio_attach(struct device *, struct device *, void *);

struct cfattach sxipio_ca = {
	sizeof (struct sxipio_softc), sxipio_match, sxipio_attach
};

struct cfdriver sxipio_cd = {
	NULL, "sxipio", DV_DULL
};

void	sxipio_attach_gpio(struct device *);
int	sxipio_pinctrl(uint32_t, void *);
void	sxipio_config_pin(void *, uint32_t *, int);
int	sxipio_get_pin(void *, uint32_t *);
void	sxipio_set_pin(void *, uint32_t *, int);

#include "sxipio_pins.h"

struct sxipio_pins {
	const char *compat;
	struct sxipio_pin *pins;
	int npins;
};

struct sxipio_pins sxipio_pins[] = {
	{
		"allwinner,sun4i-a10-pinctrl",
		sun4i_a10_pins, nitems(sun4i_a10_pins)
	},
	{
		"allwinner,sun5i-a13-pinctrl",
		sun5i_a13_pins, nitems(sun5i_a13_pins)
	},
	{
		"allwinner,sun5i-a10s-pinctrl",
		sun5i_a10s_pins, nitems(sun5i_a10s_pins)
	},
	{
		"allwinner,sun7i-a20-pinctrl",
		sun7i_a20_pins, nitems(sun7i_a20_pins)
	},
	{
		"allwinner,sun8i-h3-pinctrl",
		sun8i_h3_pins, nitems(sun8i_h3_pins)
	},
	{
		"allwinner,sun8i-h3-r-pinctrl",
		sun8i_h3_r_pins, nitems(sun8i_h3_r_pins)
	},
	{
		"allwinner,sun9i-a80-pinctrl",
		sun9i_a80_pins, nitems(sun9i_a80_pins)
	},
	{
		"allwinner,sun9i-a80-r-pinctrl",
		sun9i_a80_r_pins, nitems(sun9i_a80_r_pins)
	},
	{
		"allwinner,sun50i-a64-pinctrl",
		sun50i_a64_pins, nitems(sun50i_a64_pins)
	}
};

int
sxipio_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;
	int i;

	for (i = 0; i < nitems(sxipio_pins); i++) {
		if (OF_is_compatible(faa->fa_node, sxipio_pins[i].compat))
			return 1;
	}

	return 0;
}

void
sxipio_attach(struct device *parent, struct device *self, void *aux)
{
	struct sxipio_softc *sc = (struct sxipio_softc *)self;
	struct fdt_attach_args	*faa = aux;
	int i;

	sc->sc_iot = faa->fa_iot;
	if (bus_space_map(sc->sc_iot, faa->fa_reg[0].addr,
	    faa->fa_reg[0].size, 0, &sc->sc_ioh))
		panic("%s: bus_space_map failed!", __func__);

	clock_enable_all(faa->fa_node);
	reset_deassert_all(faa->fa_node);

	for (i = 0; i < nitems(sxipio_pins); i++) {
		if (OF_is_compatible(faa->fa_node, sxipio_pins[i].compat)) {
			sc->sc_pins = sxipio_pins[i].pins;
			sc->sc_npins = sxipio_pins[i].npins;
			break;
		}
	}

	KASSERT(sc->sc_pins);
	pinctrl_register(faa->fa_node, sxipio_pinctrl, sc);

	sc->sc_gc.gc_node = faa->fa_node;
	sc->sc_gc.gc_cookie = sc;
	sc->sc_gc.gc_config_pin = sxipio_config_pin;
	sc->sc_gc.gc_get_pin = sxipio_get_pin;
	sc->sc_gc.gc_set_pin = sxipio_set_pin;
	gpio_controller_register(&sc->sc_gc);

	config_defer(self, sxipio_attach_gpio);

	printf(": %d pins\n", sc->sc_npins);
}


int
sxipio_pinctrl(uint32_t phandle, void *cookie)
{
	struct sxipio_softc *sc = cookie;
	char func[32];
	char *names, *name;
	int port, pin, off;
	int mux, drive, pull;
	int node;
	int len;
	int i, j;
	int s;

	node = OF_getnodebyphandle(phandle);
	if (node == 0)
		return -1;

	len = OF_getprop(node, "allwinner,function", func, sizeof(func));
	if (len <= 0 || len >= sizeof(func))
		return -1;

	len = OF_getproplen(node, "allwinner,pins");
	if (len <= 0)
		return -1;

	names = malloc(len, M_TEMP, M_WAITOK);
	OF_getprop(node, "allwinner,pins", names, len);

	drive = OF_getpropint(node, "allwinner,drive", 0);
	pull = OF_getpropint(node, "allwinner,pull", 0);

	name = names;
	while (len > 0) {
		/* Lookup the pin. */
		for (i = 0; i < sc->sc_npins; i++) {
			if (strcmp(name, sc->sc_pins[i].name) == 0)
				break;
		}
		if (i >= sc->sc_npins)
			goto err;

		/* Lookup the function of the pin. */
		for (j = 0; j < nitems(sc->sc_pins[i].funcs); j++) {
			if (strcmp(func, sc->sc_pins[i].funcs[j].name) == 0)
				break;
		}
		if (j > nitems(sc->sc_pins[i].funcs))
			goto err;

		port = sc->sc_pins[i].port;
		pin = sc->sc_pins[i].pin;
		mux = sc->sc_pins[i].funcs[j].mux;

		s = splhigh();
		off = (pin & 0x7) << 2;
		SXICMS4(sc, SXIPIO_CFG(port, pin), 0x7 << off, mux << off);
		off = (pin & 0xf) << 1;
		SXICMS4(sc, SXIPIO_DRV(port, pin), 0x3 << off, drive << off);
		SXICMS4(sc, SXIPIO_PUL(port, pin), 0x3 << off, pull << off);
		splx(s);

		len -= strlen(name) + 1;
		name += strlen(name) + 1;
	}

	free(names, M_TEMP, len);
	return 0;

err:
	free(names, M_TEMP, len);
	return -1;
}

void
sxipio_config_pin(void *cookie, uint32_t *cells, int config)
{
	struct sxipio_softc *sc = cookie;
	uint32_t port = cells[0];
	uint32_t pin = cells[1];
	int mux, off;

	if (port > SXIPIO_NPORT || pin > 32)
		return;

	mux = (config & GPIO_CONFIG_OUTPUT) ? 1 : 0;
	off = (pin & 0x7) << 2;
	SXICMS4(sc, SXIPIO_CFG(port, pin), 0x7 << off, mux << off);
}

int
sxipio_get_pin(void *cookie, uint32_t *cells)
{
	struct sxipio_softc *sc = cookie;
	uint32_t port = cells[0];
	uint32_t pin = cells[1];
	uint32_t flags = cells[2];
	uint32_t reg;
	int val;

	if (port > SXIPIO_NPORT || pin > 32)
		return 0;

	reg = SXIREAD4(sc, SXIPIO_DAT(port));
	reg &= (1 << pin);
	val = (reg >> pin) & 1;
	if (flags & GPIO_ACTIVE_LOW)
		val = !val;
	return val;
}

void
sxipio_set_pin(void *cookie, uint32_t *cells, int val)
{
	struct sxipio_softc *sc = cookie;
	uint32_t port = cells[0];
	uint32_t pin = cells[1];
	uint32_t flags = cells[2];
	uint32_t reg;

	if (port > SXIPIO_NPORT || pin > 32)
		return;

	reg = SXIREAD4(sc, SXIPIO_DAT(port));
	if (flags & GPIO_ACTIVE_LOW)
		val = !val;
	if (val)
		reg |= (1 << pin);
	else
		reg &= ~(1 << pin);
	SXIWRITE4(sc, SXIPIO_DAT(port), reg);
}

/*
 * GPIO support code
 */

int	sxipio_pin_read(void *, int);
void	sxipio_pin_write(void *, int, int);
void	sxipio_pin_ctl(void *, int, int);

static const struct gpio_chipset_tag sxipio_gpio_tag = {
	.gp_pin_read = sxipio_pin_read,
	.gp_pin_write = sxipio_pin_write,
	.gp_pin_ctl = sxipio_pin_ctl
};

int
sxipio_pin_read(void *cookie, int pin)
{
	struct sxipio_gpio *gpio = cookie;
	uint32_t cells[3];

	cells[0] = gpio->port;
	cells[1] = pin;
	cells[2] = 0;

	return sxipio_get_pin(gpio->sc, cells) ? GPIO_PIN_HIGH : GPIO_PIN_LOW;
}

void
sxipio_pin_write(void *cookie, int pin, int val)
{
	struct sxipio_gpio *gpio = cookie;
	uint32_t cells[3];

	cells[0] = gpio->port;
	cells[1] = pin;
	cells[2] = 0;

	sxipio_set_pin(gpio->sc, cells, val);
}

void
sxipio_pin_ctl(void *cookie, int pin, int flags)
{
	struct sxipio_gpio *gpio = cookie;
	uint32_t cells[3];

	cells[0] = gpio->port;
	cells[1] = pin;
	cells[2] = 0;

	if (ISSET(flags, GPIO_PIN_OUTPUT))
		sxipio_config_pin(gpio->sc, cells, GPIO_CONFIG_OUTPUT);
	else
		sxipio_config_pin(gpio->sc, cells, 0);
}

void
sxipio_attach_gpio(struct device *parent)
{
	struct sxipio_softc *sc = (struct sxipio_softc *)parent;
	struct gpiobus_attach_args gba;
	uint32_t reg;
	int port, pin;
	int cfg, state;
	int i;

	for (i = 0; i < sc->sc_npins; i++) {
		/* Skip pins that have no gpio function. */
		if (strcmp(sc->sc_pins[i].funcs[0].name, "gpio_in") != 0 ||
		    strcmp(sc->sc_pins[i].funcs[1].name, "gpio_out") != 0)
			continue;

		port = sc->sc_pins[i].port;
		pin = sc->sc_pins[i].pin;

		/* Get pin configuration. */
		reg = SXIREAD4(sc, SXIPIO_CFG(port, pin));
		cfg = (reg >> (pin & 0x7)) & 0x7;

		/* Skip pins that have been assigned other functions. */
		if (cfg != SXIPIO_GPIO_IN && cfg != SXIPIO_GPIO_OUT)
			continue;

		/* Get pin state. */
		reg = SXIREAD4(sc, SXIPIO_DAT(port));
		state = (reg >> pin) & 1;

		sc->sc_gpio_pins[port][pin].pin_caps =
		    GPIO_PIN_INPUT | GPIO_PIN_OUTPUT;
		sc->sc_gpio_pins[port][pin].pin_flags =
		    GPIO_PIN_SET | (cfg ? GPIO_PIN_OUTPUT : GPIO_PIN_INPUT);
		sc->sc_gpio_pins[port][pin].pin_state = state;
		sc->sc_gpio_pins[port][pin].pin_num = pin;
	}

	for (i = 0; i <= port; i++) {
		memcpy(&sc->sc_gpio_tag[i], &sxipio_gpio_tag, sizeof(sxipio_gpio_tag));
		sc->sc_gpio_tag[i].gp_cookie = &sc->sc_gpio[i];
		sc->sc_gpio[i].sc = sc;
		sc->sc_gpio[i].port = i;

		gba.gba_name = "gpio";
		gba.gba_gc = &sc->sc_gpio_tag[i];
		gba.gba_pins = &sc->sc_gpio_pins[i][0];
		gba.gba_npins = 32;
		
#if NGPIO > 0
		config_found(&sc->sc_dev, &gba, gpiobus_print);
#endif
	}
}