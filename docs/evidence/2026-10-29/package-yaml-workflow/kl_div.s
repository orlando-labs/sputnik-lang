package torch.kl_div_packaging_test

from torch.tensor.factories import tensor, full
import torch.nn.functional as f

export main

def check(condition, message):
  raise ValueError(message) unless condition

def run(device):
  distribution = tensor([[0.25, 0.75], [0.5, 0.5]], device:)
  prediction = full([2, 2], 0 - Math.log(2.0), device:, requires_grad: true)
  reference = (distribution * (distribution.log - prediction)).sum / 2
  loss = f.kl_div(prediction, distribution, reduction: :batchmean)
  check((loss - reference).abs.item < 1e-5, "KL batchmean")
  check((f.kl_div(prediction, distribution.log, log_target: true, reduction: :batchmean) - reference).abs.item < 1e-5, "KL log-target")
  loss.backward!
  check((prediction.grad + distribution / 2).abs.sum.item < 1e-5, "KL matrix gradient")
  check(prediction.grad.device.type == device, "actual gradient device")
  scalar = tensor(0.0, device:, requires_grad: true)
  scalar_loss = f.kl_div(scalar, tensor(1.0, device:), reduction: :batchmean)
  check(scalar_loss.item == 0.0 and scalar_loss.rank == 0, "KL scalar batchmean")
  scalar_loss.backward!
  check(scalar.grad.item == -1.0 and scalar.grad.device.type == device, "KL scalar gradient")
  print "PASS kl_div " + device

def main():
  run("cpu")
  run("mps")
  0
