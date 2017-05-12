#!/bin/bash
for v in {0..3}
do
    for p in {0..2}
    do
	echo ""

	rm -f brute_force_125_perturbed.dat swift_dopair_125_perturbed.dat

  echo "./test125cells -n 6 -r 1 -d 0.1 -v $v -p $p -f perturbed"

	./test125cells -n 6 -r 1 -d 0.1 -v $v -p $p -f perturbed

	if [ -e brute_force_125_perturbed.dat ]
	then
	    python ./difffloat.py brute_force_125_perturbed.dat swift_dopair_125_perturbed.dat ./tolerance_125_perturbed.dat 6
	else
	    exit 1
        fi

    done
done
	
exit $?
