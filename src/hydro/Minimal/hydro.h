/*******************************************************************************
 * This file is part of SWIFT.
 * Copyright (c) 2016 Matthieu Schaller (matthieu.schaller@durham.ac.uk)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/
#ifndef SWIFT_MINIMAL_HYDRO_H
#define SWIFT_MINIMAL_HYDRO_H

/**
 * @file Minimal/hydro.h
 * @brief Minimal conservative implementation of SPH (Non-neighbour loop
 * equations)
 *
 * The thermal variable is the internal energy (u). Simple constant
 * viscosity term without switches is implemented. No thermal conduction
 * term is implemented.
 *
 * This corresponds to equations (43), (44), (45), (101), (103)  and (104) with
 * \f$\beta=3\f$ and \f$\alpha_u=0\f$ of Price, D., Journal of Computational
 * Physics, 2012, Volume 231, Issue 3, pp. 759-794.
 */

#include "adiabatic_index.h"
#include "approx_math.h"
#include "dimension.h"
#include "equation_of_state.h"
#include "hydro_properties.h"
#include "kernel_hydro.h"
#include "minmax.h"

/**
 * @brief Returns the internal energy of a particle
 *
 * For implementations where the main thermodynamic variable
 * is not internal energy, this function computes the internal
 * energy from the thermodynamic variable.
 *
 * @param p The particle of interest
 * @param dt Time since the last kick
 */
__attribute__((always_inline)) INLINE static float hydro_get_internal_energy(
    const struct part *restrict p, float dt) {

  return p->u + p->u_dt * dt;
}

/**
 * @brief Returns the pressure of a particle
 *
 * @param p The particle of interest
 * @param dt Time since the last kick
 */
__attribute__((always_inline)) INLINE static float hydro_get_pressure(
    const struct part *restrict p, float dt) {

  return p->force.pressure;
}

/**
 * @brief Returns the entropy of a particle
 *
 * For implementations where the main thermodynamic variable
 * is not entropy, this function computes the entropy from
 * the thermodynamic variable.
 *
 * @param p The particle of interest
 * @param dt Time since the last kick
 */
__attribute__((always_inline)) INLINE static float hydro_get_entropy(
    const struct part *restrict p, float dt) {

  const float u = p->u + p->u_dt * dt;

  return gas_entropy_from_internal_energy(p->rho, u);
}

/**
 * @brief Returns the sound speed of a particle
 *
 * @param p The particle of interest
 * @param dt Time since the last kick
 */
__attribute__((always_inline)) INLINE static float hydro_get_soundspeed(
    const struct part *restrict p, float dt) {

  return p->force.soundspeed;
}

/**
 * @brief Returns the density of a particle
 *
 * @param p The particle of interest
 */
__attribute__((always_inline)) INLINE static float hydro_get_density(
    const struct part *restrict p) {

  return p->rho;
}

/**
 * @brief Returns the mass of a particle
 *
 * @param p The particle of interest
 */
__attribute__((always_inline)) INLINE static float hydro_get_mass(
    const struct part *restrict p) {

  return p->mass;
}

/**
 * @brief Modifies the thermal state of a particle to the imposed internal
 * energy
 *
 * This overwrites the current state of the particle but does *not* change its
 * time-derivatives. Internal energy, pressure, sound-speed and signal velocity
 * will be updated.
 *
 * @param p The particle
 * @param u The new internal energy
 */
__attribute__((always_inline)) INLINE static void hydro_set_internal_energy(
    struct part *restrict p, float u) {

  p->u = u;

  /* Compute the new pressure */
  const float pressure = gas_pressure_from_internal_energy(p->rho, p->u);

  /* Compute the new sound speed */
  const float soundspeed = gas_soundspeed_from_internal_energy(p->rho, p->u);

  /* Update the signal velocity */
  const float v_sig_old = p->force.v_sig;
  const float v_sig_new = p->force.v_sig - p->force.soundspeed + soundspeed;
  const float v_sig = max(v_sig_old, v_sig_new);

  p->force.soundspeed = soundspeed;
  p->force.pressure = pressure;
  p->force.v_sig = v_sig;
}

/**
 * @brief Modifies the thermal state of a particle to the imposed entropy
 *
 * This overwrites the current state of the particle but does *not* change its
 * time-derivatives. Internal energy, pressure, sound-speed and signal velocity
 * will be updated.
 *
 * @param p The particle
 * @param S The new entropy
 */
__attribute__((always_inline)) INLINE static void hydro_set_entropy(
    struct part *restrict p, float S) {

  p->u = gas_internal_energy_from_entropy(p->rho, S);

  /* Compute the pressure */
  const float pressure = gas_pressure_from_internal_energy(p->rho, p->u);

  /* Compute the new sound speed */
  const float soundspeed = gas_soundspeed_from_internal_energy(p->rho, p->u);

  /* Update the signal velocity */
  const float v_sig_old = p->force.v_sig;
  const float v_sig_new = p->force.v_sig - p->force.soundspeed + soundspeed;
  const float v_sig = max(v_sig_old, v_sig_new);

  p->force.soundspeed = soundspeed;
  p->force.pressure = pressure;
  p->force.v_sig = v_sig;
}

/**
 * @brief Computes the hydro time-step of a given particle
 *
 * This function returns the time-step of a particle given its hydro-dynamical
 * state. A typical time-step calculation would be the use of the CFL condition.
 *
 * @param p Pointer to the particle data
 * @param xp Pointer to the extended particle data
 * @param hydro_properties The SPH parameters
 *
 */
__attribute__((always_inline)) INLINE static float hydro_compute_timestep(
    const struct part *restrict p, const struct xpart *restrict xp,
    const struct hydro_props *restrict hydro_properties) {

  const float CFL_condition = hydro_properties->CFL_condition;

  /* CFL condition */
  const float dt_cfl =
      2.f * kernel_gamma * CFL_condition * p->h / p->force.v_sig;

  return dt_cfl;
}

/**
 * @brief Initialises the particles for the first time
 *
 * This function is called only once just after the ICs have been
 * read in to do some conversions or assignments between the particle
 * and extended particle fields.
 *
 * @param p The particle to act upon
 * @param xp The extended particle data to act upon
 */
__attribute__((always_inline)) INLINE static void hydro_first_init_part(
    struct part *restrict p, struct xpart *restrict xp) {

  p->ti_begin = 0;
  p->ti_end = 0;
  xp->v_full[0] = p->v[0];
  xp->v_full[1] = p->v[1];
  xp->v_full[2] = p->v[2];
}

/**
 * @brief Prepares a particle for the density calculation.
 *
 * Zeroes all the relevant arrays in preparation for the sums taking place in
 * the various density loop over neighbours. Typically, all fields of the
 * density sub-structure of a particle get zeroed in here.
 *
 * @param p The particle to act upon
 */
__attribute__((always_inline)) INLINE static void hydro_init_part(
    struct part *restrict p) {

  p->density.wcount = 0.f;
  p->density.wcount_dh = 0.f;
  p->rho = 0.f;
  p->density.rho_dh = 0.f;
}

/**
 * @brief Finishes the density calculation.
 *
 * Multiplies the density and number of neighbours by the appropiate constants
 * and add the self-contribution term.
 * Additional quantities such as velocity gradients will also get the final
 *terms
 * added to them here.
 *
 * @param p The particle to act upon
 * @param time The current time
 */
__attribute__((always_inline)) INLINE static void hydro_end_density(
    struct part *restrict p, float time) {

  /* Some smoothing length multiples. */
  const float h = p->h;
  const float h_inv = 1.0f / h;                       /* 1/h */
  const float h_inv_dim = pow_dimension(h_inv);       /* 1/h^d */
  const float h_inv_dim_plus_one = h_inv_dim * h_inv; /* 1/h^(d+1) */

  /* Final operation on the density (add self-contribution). */
  p->rho += p->mass * kernel_root;
  p->density.rho_dh -= hydro_dimension * p->mass * kernel_root;
  p->density.wcount += kernel_root;

  /* Finish the calculation by inserting the missing h-factors */
  p->rho *= h_inv_dim;
  p->density.rho_dh *= h_inv_dim_plus_one;
  p->density.wcount *= kernel_norm;
  p->density.wcount_dh *= h_inv * kernel_gamma * kernel_norm;
}

/**
 * @brief Prepare a particle for the force calculation.
 *
 * This function is called in the ghost task to convert some quantities coming
 * from the density loop over neighbours into quantities ready to be used in the
 * force loop over neighbours. Quantities are typically read from the density
 * sub-structure and written to the force sub-structure.
 * Examples of calculations done here include the calculation of viscosity term
 * constants, thermal conduction terms, hydro conversions, etc.
 *
 * @param p The particle to act upon
 * @param xp The extended particle data to act upon
 * @param ti_current The current time (on the timeline)
 * @param timeBase The minimal time-step size
 */
__attribute__((always_inline)) INLINE static void hydro_prepare_force(
    struct part *restrict p, struct xpart *restrict xp, int ti_current,
    double timeBase) {

  /* Compute the pressure */
  const float half_dt = (ti_current - (p->ti_begin + p->ti_end) / 2) * timeBase;
  const float pressure = hydro_get_pressure(p, half_dt);

  /* Compute the sound speed */
  const float soundspeed = gas_soundspeed_from_pressure(p->rho, pressure);

  /* Compute the "grad h" term */
  const float rho_inv = 1.f / p->rho;
  const float grad_h_term =
      1.f / (1.f + hydro_dimension_inv * p->h * p->density.rho_dh * rho_inv);

  /* Update variables. */
  p->force.f = grad_h_term;
  p->force.pressure = pressure;
  p->force.soundspeed = soundspeed;
}

/**
 * @brief Reset acceleration fields of a particle
 *
 * Resets all hydro acceleration and time derivative fields in preparation
 * for the sums taking  place in the various force tasks.
 *
 * @param p The particle to act upon
 */
__attribute__((always_inline)) INLINE static void hydro_reset_acceleration(
    struct part *restrict p) {

  /* Reset the acceleration. */
  p->a_hydro[0] = 0.0f;
  p->a_hydro[1] = 0.0f;
  p->a_hydro[2] = 0.0f;

  /* Reset the time derivatives. */
  p->u_dt = 0.0f;
  p->force.h_dt = 0.0f;
  p->force.v_sig = 0.0f;
}

/**
 * @brief Predict additional particle fields forward in time when drifting
 *
 * Additional hydrodynamic quantites are drifted forward in time here. These
 * include thermal quantities (thermal energy or total energy or entropy, ...).
 *
 * @param p The particle.
 * @param xp The extended data of the particle.
 * @param dt The drift time-step.
 * @param t0 The time at the start of the drift (on the timeline).
 * @param t1 The time at the end of the drift (on the timeline).
 * @param timeBase The minimal time-step size.
 */
__attribute__((always_inline)) INLINE static void hydro_predict_extra(
    struct part *restrict p, const struct xpart *restrict xp, float dt, int t0,
    int t1, double timeBase) {

  const float h_inv = 1.f / p->h;

  /* Predict smoothing length */
  const float w1 = p->force.h_dt * h_inv * dt;
  if (fabsf(w1) < 0.2f)
    p->h *= approx_expf(w1); /* 4th order expansion of exp(w) */
  else
    p->h *= expf(w1);

  /* Predict density */
  const float w2 = -hydro_dimension * w1;
  if (fabsf(w2) < 0.2f)
    p->rho *= approx_expf(w2); /* 4th order expansion of exp(w) */
  else
    p->rho *= expf(w2);

  /* Drift the pressure */
  const float dt_entr = (t1 - (p->ti_begin + p->ti_end) / 2) * timeBase;
  const float pressure = hydro_get_pressure(p, dt_entr);

  /* Compute the new sound speed */
  const float soundspeed = gas_soundspeed_from_pressure(p->rho, pressure);

  p->force.pressure = pressure;
  p->force.soundspeed = soundspeed;
}

/**
 * @brief Finishes the force calculation.
 *
 * Multiplies the force and accelerations by the appropiate constants
 * and add the self-contribution term. In most cases, there is nothing
 * to do here.
 *
 * @param p The particle to act upon
 */
__attribute__((always_inline)) INLINE static void hydro_end_force(
    struct part *restrict p) {

  p->force.h_dt *= p->h * hydro_dimension_inv;
}

/**
 * @brief Kick the additional variables
 *
 * Additional hydrodynamic quantites are kicked forward in time here. These
 * include thermal quantities (thermal energy or total energy or entropy, ...).
 *
 * @param p The particle to act upon
 * @param xp The particle extended data to act upon
 * @param dt The time-step for this kick
 * @param half_dt The half time-step for this kick
 */
__attribute__((always_inline)) INLINE static void hydro_kick_extra(
    struct part *restrict p, struct xpart *restrict xp, float dt,
    float half_dt) {

  /* Do not decrease the energy by more than a factor of 2*/
  const float u_change = p->u_dt * dt;
  if (u_change > -0.5f * p->u)
    p->u += u_change;
  else
    p->u *= 0.5f;

  /* Do not 'overcool' when timestep increases */
  if (p->u + p->u_dt * half_dt < 0.5f * p->u) p->u_dt = -0.5f * p->u / half_dt;

  /* Compute the pressure */
  const float pressure = gas_pressure_from_internal_energy(p->rho, p->u);

  /* Compute the sound speed */
  const float soundspeed = gas_soundspeed_from_internal_energy(p->rho, p->u);

  p->force.pressure = pressure;
  p->force.soundspeed = soundspeed;
}

/**
 * @brief Converts hydro quantity of a particle at the start of a run
 *
 * This function is called once at the end of the engine_init_particle()
 * routine (at the start of a calculation) after the densities of
 * particles have been computed.
 * This can be used to convert internal energy into entropy for instance.
 *
 * @param p The particle to act upon
 */
__attribute__((always_inline)) INLINE static void hydro_convert_quantities(
    struct part *restrict p) {

  /* Compute the pressure */
  const float pressure = gas_pressure_from_internal_energy(p->rho, p->u);

  /* Compute the sound speed */
  const float soundspeed = gas_soundspeed_from_internal_energy(p->rho, p->u);

  p->force.pressure = pressure;
  p->force.soundspeed = soundspeed;
}

#endif /* SWIFT_MINIMAL_HYDRO_H */
