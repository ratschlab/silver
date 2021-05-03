# runs Varsim to generate a pattern for healthy and tumor cells based on the GRCh38_new.fa reference genome
# Inputs:
#  - the human reference genome in fasta format e.g. GRCh38_new.fa
#  - common variations on the reference genome, e.g. common_all_20180418.vcf.gz
#  - a VCF file to draw mutations for the cancer cells from, e.g. cosmic.vcf.gz
# Outputs:
#  - one fasta file for the healthy cell and one fasta for the tumor cell

# don't forget to run: "conda activate py2" before running this script

module load jdk # varsim doesn't work with openjdk

# note the use of --disable_sim; this means that no reads are generated using Varsim (because it's slow and error
# prone). Instead we generate the reads ourself directly using art_illumina in the next step

base_dir="/cluster/work/grlab/projects/projects2019-supervario/simulated_data/varsim"
out_dir=${base_dir}/healthy
mkdir -p  ${out_dir}

touch "${out_dir}/empty_file"

cmd="time ${base_dir}/varsim-0.8.4/varsim.py --id healthy --vc_in_vcf ${base_dir}/common_all_20180418.vcf.gz \
   --reference ${base_dir}/GRCh38_new.fa \
   --read_length 100 --vc_num_snp 3000000 --vc_num_ins 100000 \
   --vc_num_del 100000 --vc_num_mnp 50000 --vc_num_complex 50000 \
   --sv_num_ins 0 --sv_num_del 0 --sv_num_dup 0 --sv_num_inv 0 --sv_insert_seq ${out_dir}/empty_file \
   --sv_dgv empty_file \
   --disable_sim \
   --simulator_executable doesnt_matter_we_are_not_simulating \
   --out_dir ${out_dir} --log_dir ${out_dir}/logs/ --work_dir ${out_dir}/tmp | tee 2>&1 ${out_dir}/healthy.log"

echo "Executing: $cmd"

bsub  -J "sim-healthy" -W 1:00 -n 10 -R "rusage[mem=8000]" -R "span[hosts=1]"  -oo "${out_dir}/healthy.lsf.log" "${cmd}"


# We have to wait for the healthy.truth.vcf to be generated by the varsim simulation for healthy cells
# (because the tumor cells are based on healthy cells + cosmic mutations)


healthy_vcf="${base_dir}/healthy/healthy.truth.vcf"
if [ -f ${healthy_vcf} ]; then
  echo "Using existing ${healthy_vcf}"
else
  echo -n "Waiting for ${healthy_vcf} to be generated..."
  while [ ! -f ${healthy_vcf} ]; do sleep 10; echo -n .;  done;
  echo "done"
fi

n_tumor=1 # how many tumor cell types to generate

out_dir=${base_dir}/tumor
mkdir -p  ${out_dir}
touch "${out_dir}/empty_file"

for i in $(seq 0 n_tumor); do
  command="time python2 ${base_dir}/varsim-0.8.4/varsim_somatic.py \
          --reference ${base_dir}/GRCh38_new.fa \
          --id tumor-${i} \
          --seed ${i} \
          --som_num_snp 40000 \
          --som_num_ins 250 \
          --som_num_del 250 \
          --som_num_mnp 200 \
          --som_num_complex 200 \
          --cosmic_vcf ${base_dir}/cosmic/cosmic.vcf.gz \
          --normal_vcf ${healthy_vcf} \
          --disable_sim \
          --simulator_executable doesnt_matter_we_are_not_simulating \
          --out_dir ${out_dir} \
          --log_dir ${out_dir}/logs --work_dir ${out_dir}/tmp --sv_insert_seq ${out_dir}/empty_file"

  echo "Executing: ${command}"

  bsub  -J "sim-tumor-${i}" -W 1:00 -n 10 -R "rusage[mem=20000]" -R "span[hosts=1]"  -oo "${log_dir}/tumor-${i}.lsf.log" "${command}"
done