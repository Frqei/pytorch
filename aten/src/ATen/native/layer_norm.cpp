#include <ATen/native/layer_norm.h>

#include <ATen/AccumulateType.h>
#include <ATen/ATen.h>
#include <ATen/Config.h>
#include <ATen/CPUApplyUtils.h>
#include <ATen/NativeFunctions.h>
#include <ATen/Parallel.h>
#include <c10/util/irange.h>
#include <torch/library.h>

#include <array>
#include <functional>
#include <numeric>
#include <tuple>
#include <vector>

namespace at {
namespace native {

void layer_norm_impl_out(
    at::Tensor& out,
    at::Tensor& mean,
    at::Tensor& rstd,
    const at::Tensor& input,
    IntArrayRef normalized_shape,
    const Tensor& gamma,
    const Tensor& beta,
    double eps,
    int64_t M,
    int64_t N) {
  if (M <= 0) {
    return;
  }

  LayerNormKernel(
      input.device().type(), input, gamma, beta, M, N, eps, &out, &mean, &rstd);
  const auto input_shape = input.sizes();
  const size_t axis = input.dim() - normalized_shape.size();

  DimVector stat_shape;
  for (const auto idx : c10::irange(axis)) {
    stat_shape.emplace_back(input_shape[idx]);
  }
  for (const auto idx : c10::irange(axis, input.dim())) {
    (void)idx; // Suppress unused variable warning
    stat_shape.emplace_back(1);
  }

  mean = mean.view(stat_shape);
  rstd = rstd.view(stat_shape);
}

std::tuple<
    c10::MaybeOwned<Tensor>,
    int64_t,
    int64_t,
    c10::MaybeOwned<Tensor>,
    c10::MaybeOwned<Tensor>,
    Tensor,
    Tensor>
layer_init(
    const Tensor& input,
    IntArrayRef normalized_shape,
    const c10::optional<Tensor>& weight_opt /* optional */,
    const c10::optional<Tensor>& bias_opt /* optional */
    ) {

  c10::MaybeOwned<Tensor> weight_maybe_owned =
      at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  c10::MaybeOwned<Tensor> bias_maybe_owned =
      at::borrow_from_optional_tensor(bias_opt);
  const Tensor& bias = *bias_maybe_owned;

  auto X = input.expect_contiguous();
  auto M_N = _check_layer_norm_inputs(input, normalized_shape, weight, bias);
  auto M = M_N.first;
  auto N = M_N.second;
  auto gamma = weight.expect_contiguous();
  auto beta = bias.expect_contiguous();

  Tensor mean = at::empty({M}, X->options());
  Tensor rstd = at::empty({M}, X->options());
  return std::make_tuple(X, M, N, gamma, beta, mean, rstd);
}

Tensor& layer_norm_new_out(
    const Tensor& input,
    IntArrayRef normalized_shape,
    const c10::optional<Tensor>& weight_opt /* optional */,
    const c10::optional<Tensor>& bias_opt /* optional */,
    double eps,
    Tensor& out) {
  c10::MaybeOwned<Tensor> X, gamma, beta;
  int64_t M, N;
  Tensor mean, rstd;
  std::tie(X, M, N, gamma, beta, mean, rstd) =
      layer_init(input, normalized_shape, weight_opt, bias_opt);
  layer_norm_impl_out(
      out, mean, rstd, *X, normalized_shape, *gamma, *beta, eps, M, N);
  return out;
}

std::tuple<Tensor, Tensor, Tensor> _layer_norm(
    const Tensor& input,
    IntArrayRef normalized_shape,
    const c10::optional<Tensor>& weight_opt /* optional */,
    const c10::optional<Tensor>& bias_opt /* optional */,
    double eps) {
  c10::MaybeOwned<Tensor> X, gamma, beta;
  int64_t M, N;
  Tensor mean, rstd;
  std::tie(X, M, N, gamma, beta, mean, rstd) =
      layer_init(input, normalized_shape, weight_opt, bias_opt);
  Tensor Y = at::native::empty_like(
      *X,
      c10::nullopt /* dtype */,
      c10::nullopt /* layout */,
      c10::nullopt /* device */,
      c10::nullopt /* pin_memory */,
      at::MemoryFormat::Contiguous);

  layer_norm_impl_out(
      Y, mean, rstd, *X, normalized_shape, *gamma, *beta, eps, M, N);
  return std::make_tuple(std::move(Y), std::move(mean), std::move(rstd));
}

Tensor layer_norm_new(
    const Tensor& input,
    IntArrayRef normalized_shape,
    const c10::optional<Tensor>& weight_opt /* optional */,
    const c10::optional<Tensor>& bias_opt /* optional */,
    double eps) {
  return std::get<0>(at::native_layer_norm(input, normalized_shape, weight_opt, bias_opt, eps));
}

Tensor layer_norm(
    const Tensor& input,
    IntArrayRef normalized_shape,
    const c10::optional<Tensor>& weight_opt /* optional */,
    const c10::optional<Tensor>& bias_opt /* optional */,
    double eps,
    bool /* cudnn_enable, deprecated */) {
  return std::get<0>(at::native_layer_norm(input, normalized_shape, weight_opt, bias_opt, eps));
}

std::tuple<Tensor, Tensor, Tensor> layer_norm_backward(
    const Tensor& dY,
    const Tensor& input,
    IntArrayRef normalized_shape,
    const Tensor& mean,
    const Tensor& rstd,
    const c10::optional<Tensor>& weight_opt /* optional */,
    const c10::optional<Tensor>& bias_opt /* optional */,
    std::array<bool, 3> grad_input_mask) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned =
      at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  c10::MaybeOwned<Tensor> bias_maybe_owned =
      at::borrow_from_optional_tensor(bias_opt);
  const Tensor& bias = *bias_maybe_owned;

  auto M_N = _check_layer_norm_inputs(input, normalized_shape, weight, bias);
  auto M = M_N.first;
  auto N = M_N.second;
  auto X = input.expect_contiguous();
  auto gamma = weight.expect_contiguous();
  auto beta = bias.expect_contiguous();

  Tensor dX;
  Tensor dgamma;
  Tensor dbeta;
  if (grad_input_mask[0]) {
    dX = at::native::empty_like(
        *X,
        c10::nullopt /* dtype */,
        c10::nullopt /* layout */,
        c10::nullopt /* device */,
        c10::nullopt /* pin_memory */,
        at::MemoryFormat::Contiguous);
  }
  if (grad_input_mask[1]) {
    dgamma = M > 0 ? at::native::empty_like(
                         *gamma,
                         c10::nullopt /* dtype */,
                         c10::nullopt /* layout */,
                         c10::nullopt /* device */,
                         c10::nullopt /* pin_memory */,
                         at::MemoryFormat::Contiguous)
                   : at::native::zeros_like(
                         *gamma,
                         c10::nullopt /* dtype */,
                         c10::nullopt /* layout */,
                         c10::nullopt /* device */,
                         c10::nullopt /* pin_memory */,
                         at::MemoryFormat::Contiguous);
  }
  if (grad_input_mask[2]) {
    dbeta = M > 0 ? at::native::empty_like(
                        *beta,
                        c10::nullopt /* dtype */,
                        c10::nullopt /* layout */,
                        c10::nullopt /* device */,
                        c10::nullopt /* pin_memory */,
                        at::MemoryFormat::Contiguous)
                  : at::native::zeros_like(
                        *beta,
                        c10::nullopt /* dtype */,
                        c10::nullopt /* layout */,
                        c10::nullopt /* device */,
                        c10::nullopt /* pin_memory */,
                        at::MemoryFormat::Contiguous);
  }
  if (M > 0) {
    LayerNormBackwardKernel(
        dY.device().type(),
        dY,
        *X,
        mean,
        rstd,
        *gamma,
        M,
        N,
        &dX,
        &dgamma,
        &dbeta);
  }
  return std::make_tuple(std::move(dX), std::move(dgamma), std::move(dbeta));
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_DISPATCH(LayerNormKernel);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_DISPATCH(LayerNormBackwardKernel);

// Ported from pytorch/xla repo
std::tuple<Tensor, Tensor, Tensor> math_native_layer_norm(
    const Tensor& input,
    IntArrayRef normalized_shape,
    const c10::optional<Tensor>& weight_opt,
    const c10::optional<Tensor>& bias_opt,
    double eps) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned =
      at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  c10::MaybeOwned<Tensor> bias_maybe_owned =
      at::borrow_from_optional_tensor(bias_opt);
  const Tensor& bias = *bias_maybe_owned;

  auto M_N = _check_layer_norm_inputs(input, normalized_shape, weight, bias);
  auto M = M_N.first;
  auto X = input.expect_contiguous();
  auto gamma = weight.expect_contiguous();

  auto input_shape = input.sizes();
  const auto input_ndim = input.dim();
  const int normalized_ndim = normalized_shape.size();
  // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
  const int axis = input_ndim - normalized_ndim;
  at::Tensor input_reshaped = input.view({1, M, -1});
  // Unlike Batch Normalization, which applies scalar scale and bias for each
  // entire channel/plane with the affine option, Layer Normalization applies
  // per-element scale and bias. E.g. For input {N, C, H, W}, weight for
  // batchnorm has shape {C} while weight for layernorm has shape {H, W} or {W}.
  auto outputs = at::native_batch_norm(
      input_reshaped,
      /*weight=*/{},
      /*bias=*/{},
      /*running_mean=*/{},
      /*running_var=*/{},
      /*training=*/true,
      /*momentum=*/0,
      eps);
  at::Tensor out = std::get<0>(outputs);
  out = out.view(input_shape);
  if (weight.defined() && bias.defined()) {
    out = bias.addcmul(out, weight, 1);
  } else if (weight.defined()) {
    out = out.mul(weight);
  } else if (bias.defined()) {
    out = out.add(bias);
  }
  at::Tensor mean = std::get<1>(outputs);
  at::Tensor rstd = std::get<2>(outputs);
  std::vector<int64_t> stat_shape;
  for (const auto idx : c10::irange(axis)) {
    stat_shape.push_back(input_shape[idx]);
  }
  for (const auto idx : c10::irange(axis, input.dim())) {
    (void)idx; // Suppress unused variable
    stat_shape.push_back(1);
  }
  mean = mean.view(stat_shape);
  rstd = rstd.view(stat_shape);
  return std::make_tuple(out, mean, rstd);
}
} // namespace native
} // namespace at
