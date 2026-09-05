<div align="center">

<h1><u>VOCA</u> 📹</h1>
<h2>Visual Odometry with Codec Awareness</h2>

<p>
  <a href="https://www.linkedin.com/in/nourihilscher/">Nouri Alexander Hilscher*</a>
  &nbsp;·&nbsp;
  <a href="https://cvg.cit.tum.de/members/mayom">Mateo de Mayo*</a>
  &nbsp;·&nbsp;
  <a href="https://dominikmuhle.github.io/">Dominik Muhle</a>
  &nbsp;·&nbsp;
  <a href="https://www.linkedin.com/in/christoph-otten-genannt-hermes-4106662b7/">Christoph Otten genannt Hermes</a>
  &nbsp;·&nbsp;
  <a href="https://cvg.cit.tum.de/members/cremers">Daniel Cremers</a>
</p>

<h3>ECCV 2026 &nbsp; <strong>🌟 SPOTLIGHT 🌟</strong></h3>

<p>
  <a href="https://arxiv.org/abs/2607.00189"><strong>Paper</strong></a>
  &nbsp;|&nbsp;
  <a href="https://tum-vision.github.io/voca"><strong>Project Page</strong></a>
</p>

<img src="docs/assets/voca-teaser.gif" alt="VOCA teaser" width="100%">

</div>




## 📋 Abstract

Camera pose estimation from image streams is a critical component of spatial world models that integrate perception for planning and decision making. Nearly all visual odometry (VO) and SLAM systems have focused on datasets containing raw and uncompressed videos. Many working systems, instead, use ubiquitous hardware units to compress and decode video streams efficiently, saving orders of magnitude in space and bandwidth. However, this lossy compression introduces visual artifacts that hinder the performance of traditional tracking systems. In this work, we present VOCA, a causal stereo visual-odometry method that exploits codec information to improve tracking performance. We achieve state-of-the-art performance on causal VO for relative trajectory error, efficiency, and absolute trajectory error on compressed streams.


## 📌 Release Status

- [X] [Paper (arXiv)](https://arxiv.org/abs/2607.00189)
- [ ] Code
- [ ] Ablations
- [ ] Evaluation results




## 📚 BibTeX
If you find our work useful, please consider citing our paper:
```bibtex
@inproceedings{hilscher2026Voca,
  author    = {Hilscher, Nouri Alexander and de Mayo, Mateo and Muhle, Dominik and Otten genannt Hermes, Christoph and Cremers, Daniel},
  title     = {VOCA: Visual Odometry with Codec Awareness},
  booktitle = {European Conference on Computer Vision (ECCV 2026)},
  year      = {2026},
}
```

## 🗣️ Acknowledgements
This work was supported by the European Research
Council (ERC) Advanced Grant SIMULACRON, by the DFG project CR 250/26-
1 “4D-YouTube”, by the GNI Project “AI4Twinning”, and by the Munich Center
for Machine Learning.

Our work builds on the [Basalt codebase](https://github.com/VladyslavUsenko/basalt)
and the newer implementation described in the
[Monado SLAM Dataset paper](https://arxiv.org/pdf/2508.00088). We also use the
[Monado SLAM Dataset](https://huggingface.co/datasets/collabora/monado-slam-datasets).
