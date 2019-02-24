import matplotlib
matplotlib.use("Agg")
from pylab import *
import h5py
import os.path
import numpy as np
import glob

directories = ["continuous_old_v3/", "stochastic_dT_5.5_old/", "stochastic_dT_6.5_old/", "stochastic_dT_7.5_old/"]
colours = ["k","r","g","b"]
names = ["continuous","$dT = 10^{5.5}$","$dT = 10^{6.5}$","$dT = 10^{7.5}$"]

# Number of snapshots and elements
n_snapshots = 53
n_elements = 9

# Plot parameters
params = {'axes.labelsize': 10,
'axes.titlesize': 10,
'font.size': 9,
'legend.fontsize': 9,
'xtick.labelsize': 10,
'ytick.labelsize': 10,
'text.usetex': True,
 'figure.figsize' : (3.15,3.15),
'figure.subplot.left'    : 0.15,
'figure.subplot.right'   : 0.95,
'figure.subplot.bottom'  : 0.15,
'figure.subplot.top'     : 0.95,
'figure.subplot.wspace'  : 0.21,
'figure.subplot.hspace'  : 0.19,
'lines.markersize' : 6,
'lines.linewidth' : 2.,
'text.latex.unicode': True
}

rcParams.update(params)
rc('font',**{'family':'sans-serif','sans-serif':['Times']})

# Read the simulation data
sim = h5py.File(directories[0]+"stellar_evolution_0000.hdf5", "r")
boxSize = sim["/Header"].attrs["BoxSize"][0]
scheme = sim["/HydroScheme"].attrs["Scheme"][0]
kernel = sim["/HydroScheme"].attrs["Kernel function"][0]
neighbours = sim["/HydroScheme"].attrs["Kernel target N_ngb"][0]
eta = sim["/HydroScheme"].attrs["Kernel eta"][0]
alpha = sim["/HydroScheme"].attrs["Alpha viscosity"][0]
H_mass_fraction = sim["/HydroScheme"].attrs["Hydrogen mass fraction"][0]
H_transition_temp = sim["/HydroScheme"].attrs["Hydrogen ionization transition temperature"][0]
T_initial = sim["/HydroScheme"].attrs["Initial temperature"][0]
T_minimal = sim["/HydroScheme"].attrs["Minimal temperature"][0]
git = sim["Code"].attrs["Git Revision"]
star_initial_mass = sim["/PartType4/Masses"][0]

# Cosmological parameters
H_0 = sim["/Cosmology"].attrs["H0 [internal units]"][0]
gas_gamma = sim["/HydroScheme"].attrs["Adiabatic index"][0]

# Units
unit_length_in_cgs = sim["/Units"].attrs["Unit length in cgs (U_L)"]
unit_mass_in_cgs = sim["/Units"].attrs["Unit mass in cgs (U_M)"]
unit_time_in_cgs = sim["/Units"].attrs["Unit time in cgs (U_t)"]
unit_vel_in_cgs = unit_length_in_cgs / unit_time_in_cgs
unit_energy_in_cgs = unit_mass_in_cgs * unit_vel_in_cgs * unit_vel_in_cgs
unit_length_in_si = 0.01 * unit_length_in_cgs
unit_mass_in_si = 0.001 * unit_mass_in_cgs
unit_time_in_si = unit_time_in_cgs
Myr_in_cgs = 3.154e13

# Find out how many particles (gas and star) we have
n_parts = sim["/Header"].attrs["NumPart_Total"][0]
n_sparts = sim["/Header"].attrs["NumPart_Total"][4]

# Declare arrays for data
masses = zeros((n_parts,n_snapshots))
star_masses = zeros((n_sparts,n_snapshots))
mass_from_AGB = zeros((n_parts,n_snapshots))
metal_mass_frac_from_AGB = zeros((n_parts,n_snapshots))
mass_from_SNII = zeros((n_parts,n_snapshots))
metal_mass_frac_from_SNII = zeros((n_parts,n_snapshots))
mass_from_SNIa = zeros((n_parts,n_snapshots))
metal_mass_frac_from_SNIa = zeros((n_parts,n_snapshots))
iron_mass_frac_from_SNIa = zeros((n_parts,n_snapshots))
metallicity = zeros((n_parts,n_snapshots))
abundances = zeros((n_parts,n_elements,n_snapshots))
internal_energy = zeros((n_parts,n_snapshots))
coord_parts = zeros((n_parts,3,n_snapshots))
velocity_parts = zeros((n_parts,3,n_snapshots))
speed_parts = zeros((n_parts,n_snapshots))
coord_sparts = zeros((3,n_snapshots))
smoothing_length_parts = zeros((n_parts,n_snapshots))
distances = zeros((n_parts,n_snapshots))
smoothing_length_sparts = zeros(n_snapshots)
time = zeros(n_snapshots)
vel2 = zeros((n_parts,n_snapshots))

# Create figure for plotting energy evolution
figure()
subplot(111)

count = 0
for dir in directories:
	files = glob.glob(dir+'stellar_evolution_*.hdf5')
	#snapshots = zeros(len(files))
	#for file in files:
	#	snapshots[:] = int(file.replace(dir+'stellar_evolution_','').replace('.hdf5',''))
	#n_snapshots = np.max(snapshots) + 1
	n_snapshots = len(files)
	print(n_snapshots)
	# Read fields we are checking from snapshots
	for i in range(n_snapshots):
		sim = h5py.File(dir+"stellar_evolution_%04d.hdf5"%i, "r")
		print('reading snapshot '+str(i))
		#abundances[:,:,i] = sim["/PartType0/ElementAbundance"]
		#metallicity[:,i] = sim["/PartType0/Metallicity"]
		masses[:,i] = sim["/PartType0/Masses"]
		star_masses[:,i] = sim["/PartType4/Masses"]
		#mass_from_AGB[:,i] = sim["/PartType0/TotalMassFromAGB"]
		#metal_mass_frac_from_AGB[:,i] = sim["/PartType0/MetalMassFracFromAGB"]
		#mass_from_SNII[:,i] = sim["/PartType0/TotalMassFromSNII"]
		#metal_mass_frac_from_SNII[:,i] = sim["/PartType0/MetalMassFracFromSNII"]
		#mass_from_SNIa[:,i] = sim["/PartType0/TotalMassFromSNIa"]
		#metal_mass_frac_from_SNIa[:,i] = sim["/PartType0/MetalMassFracFromSNIa"]
		#iron_mass_frac_from_SNIa[:,i] = sim["/PartType0/IronMassFracFromSNIa"]
		internal_energy[:,i] = sim["/PartType0/InternalEnergy"]
		velocity_parts[:,:,i] = sim["/PartType0/Velocities"]
		#coord_parts[:,:,i] = sim["/PartType0/Coordinates"]
		#coord_sparts[:,i] = sim["/PartType4/Coordinates"]
		#smoothing_length_parts[:,i] = sim["/PartType0/SmoothingLength"]
		#smoothing_length_sparts[i] = sim["/PartType4/SmoothingLength"][0]
		time[i] = sim["/Header"].attrs["Time"][0]
	
	vel2[:,:] = velocity_parts[:,0,:]*velocity_parts[:,0,:] + velocity_parts[:,1,:]*velocity_parts[:,1,:] + velocity_parts[:,2,:]*velocity_parts[:,2,:]
	total_kinetic_energy = np.sum(np.multiply(vel2,masses)*0.5,axis = 0)
	total_energy = np.sum(np.multiply(internal_energy,masses),axis = 0)
	plot(time[0:n_snapshots]*unit_time_in_cgs/Myr_in_cgs, (total_energy[0:n_snapshots] + total_kinetic_energy[0:n_snapshots])*unit_energy_in_cgs,color=colours[count], label=names[count], linewidth=0.5)
	count += 1

xlim([0,50])
ylim([0,1e56])
xlabel("Time (Myr)")
ylabel("total energy (erg)")
legend()
savefig("energy.png", dpi=200)

# Check that the total amount of enrichment is as expected.
# Define tolerance
eps = 0.01

# Stochastic heating
#vel2 = zeros((n_parts,n_snapshots))
#vel2[:,:] = velocity_parts[:,0,:]*velocity_parts[:,0,:] + velocity_parts[:,1,:]*velocity_parts[:,1,:] + velocity_parts[:,2,:]*velocity_parts[:,2,:]
#total_kinetic_energy = np.sum(np.multiply(vel2,masses)*0.5,axis = 0)
#total_energy = np.sum(np.multiply(internal_energy,masses),axis = 0)
#total_energy_released = total_energy[n_snapshots-1] - total_energy[0] + total_kinetic_energy[n_snapshots-1] - total_kinetic_energy[0]

# Find out how many total sn should go off in simulation time
#feedback_data = "feedback_properties.dat"
#heating_probability = 0
#energy_released = 0
#num_heating_events = 0
#with open(feedback_data) as f:
#	num_sn = float(f.readline().strip())
#	total_time = float(f.readline().strip())
#	# Find out how many heating  events occurred 
#	while True:
#		num = f.readline().strip()
#		if not num:
#			break
#		heating_probability += float(num.split()[0])
#		energy_released += float(num.split()[1])
#		num_heating_events += 1
#total_sn = num_sn * time[n_snapshots-1]/total_time
#print("total sn " + str(total_sn) + " fraction time elapsed " + str(time[n_snapshots-1]/total_time))
#
## Calculate energy released
#energy_per_sn = 1.0e51 / unit_energy_in_cgs
#expected_energy_released = total_sn * energy_per_sn
#print("heating probability " + str(heating_probability) + " energy released " + str(energy_released) + " num_heating_events*energy_per_sn " + str(num_heating_events*energy_per_sn) + " expected energy released " + str(expected_energy_released))
#
## Did we get it right?
#if abs(total_energy_released - expected_energy_released)/expected_energy_released < eps:
#	print("total stochastic energy release consistent with expectation. total stochastic energy release "+str(total_energy_released)+" expected "+ str(expected_energy_released) + " initial total internal energy "+ str(total_energy[0] + total_kinetic_energy[0]))
#else:
#	print("total stochastic energy release "+str(total_energy_released)+" expected "+ str(expected_energy_released) + " initial total internal energy "+ str(total_energy[0] + total_kinetic_energy[0]) + " energy change fraction of total " + str(total_energy_released/(total_energy[0]+total_kinetic_energy[0])))


