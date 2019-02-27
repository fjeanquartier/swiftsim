"""
Creates initial conditiosn for the ContactDiscontinuty_1D test.
Requires the swiftsimio library.
"""

import numpy as np
import unyt


def get_particle_positions(box_length: float, n_part: int) -> np.array:
    """
    Gets the particle positions evenly spaced along a _periodic_ box.
    """

    dx = box_length / float(n_part)
    x = np.arange(n_part, dtype=float) * dx + (0.5 * dx)

    return x


def get_particle_u(low: float, high: float, n_part: int) -> np.array:
    """
    Gets the particle internal energies, which alternate between low
    and high. Make sure n_part is even.
    """

    indicies = np.arange(n_part)
    u = unyt.unyt_array(np.empty(n_part, dtype=float), low.units)
    u[...] = low
    u[indicies % 2] = high

    return u


def get_particle_masses(mass: float, n_part: int) -> np.array:
    """
    Gets the particle masses.
    """

    m = unyt.unyt_array(np.empty(n_part, dtype=float), mass.units)
    m[...] = mass

    return m


def get_particle_hsml(box_length: float, n_part: int) -> np.array:
    """
    Gets the particle smoothing lengths based on their MIPS.
    """

    mips = box_length / float(n_part)
    hsml = unyt.unyt_array(np.empty(n_part, dtype=float), box_length.units)
    hsml[...] = mips

    return hsml


if __name__ == "__main__":
    import argparse as ap
    from swiftsimio import Writer

    parser = ap.ArgumentParser(
        description="Makes initial conditions for the ContactDiscontinuity_1D test."
    )

    parser.add_argument(
        "-n",
        "--npart",
        help="Number of particles in the box. Make sure this is even. Default: 900",
        type=int,
        default=900,
    )

    parser.add_argument(
        "-m",
        "--mass",
        help="Particle mass in grams. Default: 1.0",
        type=float,
        default=1.0,
    )

    parser.add_argument(
        "-l",
        "--low",
        help="Low value for the internal energy (in ergs/g). Default: 1.0",
        type=float,
        default=1.0,
    )

    parser.add_argument(
        "-t",
        "--high",
        help="Top/high value for the internal energy (in ergs/g). Default: 1.0",
        type=float,
        default=10.0,
    )

    parser.add_argument(
        "-b", "--boxsize", help="Boxsize in cm. Default: 1.0", type=float, default=1.0
    )

    args = vars(parser.parse_args())
    boxsize = args["boxsize"] * unyt.cm
    n_part = args["npart"]
    mass = args["mass"] * unyt.g
    low = args["low"] * unyt.erg / unyt.g
    high = args["high"] * unyt.erg / unyt.g

    if not (n_part % 2 == 0):
        raise AttributeError("Please ensure --npart is even.")

    cgs = unyt.unit_systems.cgs_unit_system

    boxsize = 1.0 * unyt.cm

    writer = Writer(cgs, boxsize, dimension=1)

    writer.gas.coordinates = get_particle_positions(boxsize, n_part)
    writer.gas.velocities = np.zeros(n_part) * unyt.cm / unyt.s
    writer.gas.masses = get_particle_masses(mass, n_part)
    writer.gas.internal_energy = get_particle_u(low, high, n_part)
    writer.gas.smoothing_length = get_particle_hsml(boxsize, n_part)

    writer.write("contact.hdf5")
