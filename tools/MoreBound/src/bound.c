#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "globalvars.h"
#include "particledata.h"

#ifdef TRACY_ENABLE

#include <tracy/TracyC.h>

#else

#define TracyCZone(ctx, active)
#define TracyCZoneN(ctx, name, active)
#define TracyCZoneEnd(ctx)
#define TracyCFrameMarkEnd(a)
#define TracyCFrameMarkNamed(a)

#endif

#define N_THREADS 6
#define N_REMNANTS 3

#define IS_UNBOUND == 0
#define IS_BOUND > 0

int calculate_material(int id) {
  for (int i = 1; i <= nMat; i++) {
    if (id < i * idskip)
      return i;
  }
  return nMat;
}

/*
 * Calculates the potential of a particle w.r.t. all particles /not/ bound by a
 * larger remnant
 */
double calculate_potential(const ParticleData *pd, size_t index, int remnant) {
  // for the largest remnant this is just the already-known potential
  if (remnant == 1) {
    return pd->potential[index];
  }

  double potential = 0;
  // loop through particles, adding in the potentials
  FVec3 *reference = pd_get_pos(pd, index);
  for (size_t i = 0; i < pd->total_number; ++i) {
    if (pd->bnd[i] IS_BOUND && pd->bnd[i] < remnant) {
      continue;
    }
    if (i == index) {
      continue;
    }

    float seperation = fvec_square_distance(reference, pd_get_pos(pd, i));
    // TODO: is this right? only update potential if they are really far away?
    if (seperation > 1e10) {
      potential -= G * pd->mass[i] / sqrtf(seperation);
    }
  }

  return potential;
}

FVec3 fvec_mult(FVec3 v, float m) {
  v.x *= m;
  v.y *= m;
  v.z *= m;
  return v;
}

void fvec_muladd(FVec3 *out, const FVec3 *v, float m) {
  out->x += v->x * m;
  out->y += v->y * m;
  out->z += v->z * m;
}

typedef struct _CentreOfMass {
  FVec3 pos, vel;
} CentreOfMass;

void update_centre_of_mass(CentreOfMass *com, const ParticleData *pd,
                           size_t i) {
  fvec_muladd(&com->pos, pd_get_pos(pd, i), pd->mass[i]);
  fvec_muladd(&com->vel, pd_get_vel(pd, i), pd->mass[i]);
}

typedef struct _PotDiffThreadStorage {
  ParticleData *pd;
  int remnant;

  pthread_mutex_t *mutex;
  size_t *jobcount;
  size_t total_jobs;
  size_t batch_size;
} PotDiffThreadStorage;

void update_potential(ParticleData *pd, size_t start, size_t end, int remnant) {
  TracyCZone(uppotctx, 1);
  for (size_t i = start; i < end; ++i) {
    const FVec3 *reference = pd_get_pos(pd, i);
    // if particle was bound to the previous remnant
    // update the potential of all other particles to not include bound particle
    // anymore
    if (remnant > 1 && pd->bnd[i] == remnant - 1) {
      TracyCZoneN(potctx, "potential_update", 1);
      for (size_t j = 0; j < pd->total_number; ++j) {
        float seperation = fvec_square_distance(reference, pd_get_pos(pd, j));
        if (seperation > 1e10) {
          // add potential
          pd->potential[j] += G * pd->mass[i] / sqrtf(seperation);
        }
      }
      TracyCZoneEnd(potctx);
    }
  }
  TracyCZoneEnd(uppotctx);
}

// update particle data potential by removing the potential difference of those
// that are bound
void *worker_update_potential(void *arg) {
  PotDiffThreadStorage *ts = (PotDiffThreadStorage *)arg;
  ParticleData *pd = ts->pd;
  const int remnant = ts->remnant;

  size_t i = 0;
  while (i < ts->total_jobs) {
    // aquire lock
    if (pthread_mutex_lock(ts->mutex)) {
      // if we can't get it, try again
      continue;
    }

    // get and increment counter
    i = *(ts->jobcount);
    *(ts->jobcount) += 1;

    // release lock
    pthread_mutex_unlock(ts->mutex);

    if (i >= ts->total_jobs)
      break;

    const size_t start = ts->batch_size * i;
    update_potential(pd, start, start + ts->batch_size, remnant);
  }

  return NULL;
}

size_t find_index_minimum(float *ptr, size_t N) {
  TracyCZone(minctx, 1);
  size_t index = 0;
  double minimum = ptr[index];
  for (size_t i = 0; i < N; ++i) {
    if (ptr[i] < minimum) {
      index = i;
      minimum = ptr[i];
    }
  }
  TracyCZoneEnd(minctx);
  return index;
}

size_t find_particle_potential_min(ParticleData *pd, int remnant) {
  if (remnant == 1)
    goto FINDMIN;

  // worker pool
  pthread_t threads[N_THREADS];
  pthread_mutex_t mutex;
  PotDiffThreadStorage storage[N_THREADS];
  const size_t batch_size = 2048;

  // calculate size and number of jobs
  size_t jobcount = 0;
  const size_t jobs = pd->total_number / batch_size;
  const size_t rem = pd->total_number - (jobs * batch_size);
  const size_t workers = jobs > N_THREADS ? N_THREADS : jobs;

  if (pthread_mutex_init(&mutex, NULL)) {
    printf("Failed to init mutex");
    exit(3);
  }

  size_t i;
  // launch all workers
  for (i = 0; i < workers; ++i) {
    storage[i] = (PotDiffThreadStorage){pd,        remnant, &mutex,
                                        &jobcount, jobs,    batch_size};
    pthread_create(&threads[i], NULL, worker_update_potential, &storage[i]);
  }

  // calculate the remainder in main thread
  update_potential(pd, pd->total_number - rem, pd->total_number, remnant);

  // join all workers
  for (i = 0; i < workers; ++i) {
    pthread_join(threads[i], NULL);
  }

FINDMIN:
  return find_index_minimum(pd->potential, pd->total_number);
}

float fvec_square_distance(const FVec3 *vel1, const FVec3 *vel2) {
  float v1 = (vel1->x - vel2->x);
  float v2 = (vel1->y - vel2->y);
  float v3 = (vel1->z - vel2->z);
  return (v1 * v1) + (v2 * v2) + (v3 * v3);
}

FVec3 fvec_zero() { return (FVec3){0, 0, 0}; }

size_t find_and_update_bound(const ParticleData *pd, CentreOfMass *weighted,
                             CentreOfMass data, double *total_bound_mass,
                             int remnant) {
  TracyCZone(findctx, 1);
  size_t num_bound = 0;
  double mass_total = *total_bound_mass;

  for (size_t i = 0; i < pd->total_number; ++i) {
    if (pd->bnd[i] IS_BOUND) {
      continue;
    }

    float rel_velocity = fvec_square_distance(pd_get_vel(pd, i), &data.vel);
    float distance = sqrt(fvec_square_distance(pd_get_pos(pd, i), &data.pos));

    // kinetic & potential energy in this COM frame
    float ke = 0.5 * pd->mass[i] * rel_velocity;
    float pe = -G * mass_total * pd->mass[i] / distance;

    if (ke + pe < 0) {
      // particle is bound
      pd->bnd[i] = remnant;
      update_centre_of_mass(weighted, pd, i);
      num_bound += 1;
      mass_total += pd->mass[i];
    }
  }

  *total_bound_mass = mass_total;
  TracyCZoneEnd(findctx);
  return num_bound;
}

typedef struct _Remnant {
  int id;
  size_t total_bound;
  double mass;
} Remnant;

Remnant calculate_remnant(ParticleData *pd, int remnant) {
  TracyCFrameMarkNamed("remnant");
  TracyCZone(remnantctx, 1);
  CentreOfMass weighted = (CentreOfMass){fvec_zero(), fvec_zero()};

  // number of particles bound to the current remnant
  int nbound = 1;

  printf("Finding potential minimum\n");
  size_t seedindex = find_particle_potential_min(pd, remnant);
  printf("Seedindex found %ld\n", seedindex);

  // set the seed as bound to current remnant
  pd->bnd[seedindex] = remnant;

  // update the center of mass
  update_centre_of_mass(&weighted, pd, seedindex);

  double old_bound_mass, bound_mass = pd->mass[seedindex];
  double fractional_difference;

  int count = 0;
  while (count < maxit) {
    old_bound_mass = bound_mass;

    double ibm = 1 / bound_mass;

    // search through particles, finding ones bound w.r.t. the seed
    CentreOfMass current = (CentreOfMass){
        fvec_mult(weighted.pos, ibm),
        fvec_mult(weighted.vel, ibm),
    };
    nbound +=
        find_and_update_bound(pd, &weighted, current, &bound_mass, remnant);
    ++count;

    fractional_difference =
        (fabs(old_bound_mass - bound_mass) / old_bound_mass);
    if (fractional_difference < tol)
      break;
  }

  // print status of iteration
  printf("REMNANT %d: Iteration completed in %d/%d steps. Mass of "
         "%gg.\nNumber of particles: %d\nFractional difference: %g\n\n",
         remnant, count, maxit, bound_mass, nbound, fractional_difference);
  TracyCZoneEnd(remnantctx);
  TracyCFrameMarkEnd("remnant");
  return (Remnant){remnant, nbound, bound_mass};
}

int compare_remnant_mass(const void *ptr_a, const void *ptr_b) {
  Remnant *a = (Remnant *)ptr_a;
  Remnant *b = (Remnant *)ptr_b;
  if (a->mass == b->mass)
    return 0;
  if (a->mass > b->mass)
    return -1;
  return 1;
}

/*
 * Finds particle closest to potential minimum, then uses this as a seed for
 * the iterative procedure.
 * Given a selection of particles known to be bound, find all particles
 * bound w.r.t. them & add these to the list of bound particles.
 * Repeat until the bound mass change <= tol between iterations or until
 * maxit is exceeded
 */
void calculate_binding(ParticleData *pd) {
  TracyCZone(bindingctx, 1);
  // set all particles as unbound
  size_t i;
  for (i = 0; i < pd->total_number; ++i) {
    pd->bnd[i] = 0;
  }

  int total_bound = 0;
  // ordinal representing the remnant that we're finding

  Remnant remnants[N_REMNANTS];
  for (int id = 0; id < N_REMNANTS || total_bound >= minparts; ++id) {
    remnants[id] = calculate_remnant(pd, id + 1);
  }

  // now sort in order of largest to smallest
  qsort(remnants, N_REMNANTS, sizeof(Remnant), &compare_remnant_mass);

  int lookup[N_REMNANTS];
  for (i = 0; i < N_REMNANTS; ++i) {
    size_t index = remnants[i].id - 1;
    lookup[index] = i;
    if (index != i) {
      printf("new ordering: %ld -> %ld (mass = %g)\n", index + 1, i + 1,
             remnants[i].mass);
    }
  }

  // update them with negative values, and then we multiply at the end to revert
  for (i = 0; i < pd->total_number; ++i) {
    if (pd->bnd[i] IS_BOUND) {
      pd->bnd[i] = -lookup[pd->bnd[i] - 1];
    }
  }
  for (i = 0; i < pd->total_number; ++i) {
    pd->bnd[i] *= -1;
  }
  TracyCZoneEnd(bindingctx);
  return;
}
