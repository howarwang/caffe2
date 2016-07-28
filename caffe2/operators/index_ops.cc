#include <mutex>
#include <atomic>
#include <unordered_map>
#include <vector>
#include "caffe2/core/operator.h"
#include "caffe2/core/tensor.h"
#include <limits>

namespace caffe2 {
namespace {
using IndexKeyTypes = TensorTypes<int32_t, int64_t, std::string>;
using TIndexValue = int64_t;
}  // namespace

struct IndexBase {
 public:
  IndexBase(TIndexValue maxElements, const TypeMeta& type)
    : maxElements_{maxElements}
    , meta_(type)
    , frozen_{false} {}
  void Freeze() { frozen_ = true; }
  virtual ~IndexBase() {}
  const TypeMeta& Type() const { return meta_; }
  TIndexValue Size() {
    std::lock_guard<std::mutex> guard(dictMutex_);
    return nextId_;
  }

 protected:
  int64_t maxElements_;
  TypeMeta meta_;
  TIndexValue nextId_{1}; // guarded by dictMutex_
  std::atomic<bool> frozen_{false};
  std::mutex dictMutex_;
};

template<typename T>
struct Index: IndexBase {
  explicit Index(TIndexValue maxElements)
    : IndexBase(maxElements, TypeMeta::Make<T>()) {}

  bool Get(const T* keys, TIndexValue* values, size_t numKeys) {
    if (frozen_) {
      FrozenGet(keys, values, numKeys);
      return true;
    }
    std::lock_guard<std::mutex> lock(dictMutex_);
    for (int i = 0; i < numKeys; ++i) {
      auto it = dict_.find(keys[i]);
      if (it != dict_.end()) {
        values[i] = it->second;
      } else if (nextId_ < maxElements_) {
        auto newValue = nextId_++;
        dict_.insert({keys[i], newValue});
        values[i] = newValue;
      } else {
        return false;
      }
    }
    return true;
  }

  bool Load(const T* keys, size_t numKeys) {
    CAFFE_ENFORCE(
        numKeys <= maxElements_,
        "Cannot load index: Tensor is larger than max_elements.");
    decltype(dict_) dict;
    for (int i = 0; i < numKeys; ++i) {
      CAFFE_ENFORCE(
          dict.insert({keys[i], i + 1}).second,
          "Repeated elements found: cannot load into dictionary.");
    }
    // assume no `get` is inflight while this happens
    {
      std::lock_guard<std::mutex> lock(dictMutex_);
      // let the old dict get destructed outside of the lock
      dict_.swap(dict);
      nextId_ = numKeys + 1;
    }
    return true;
  }

  template<typename Ctx>
  bool Store(Tensor<Ctx>* out) {
    std::lock_guard<std::mutex> lock(dictMutex_);
    out->Resize(nextId_ - 1);
    auto outData = out->template mutable_data<T>();
    for (const auto& entry : dict_) {
      outData[entry.second - 1] = entry.first;
    }
    return true;
  }

 private:
  void FrozenGet(const T* keys, TIndexValue* values, size_t numKeys) {
    for (int i = 0; i < numKeys; ++i) {
      auto it = dict_.find(keys[i]);
      values[i] = it != dict_.end() ? it->second : 0;
    }
  }

  std::unordered_map<T, TIndexValue> dict_;
};

template<class T>
class IndexCreateOp: public Operator<CPUContext> {
 public:
  IndexCreateOp(const OperatorDef& operator_def, Workspace* ws)
   : Operator(operator_def, ws)
   // TODO(azzolini): figure out how to get to sizes larger than int32
   , maxElements_(OperatorBase::GetSingleArgument<int>(
      "max_elements", std::numeric_limits<int>::max())) {}

  bool RunOnDevice() override {
    *OperatorBase::Output<std::unique_ptr<IndexBase>>(0) =
      std::unique_ptr<IndexBase>(new Index<T>(maxElements_));
    return true;
  }

 private:
  TIndexValue maxElements_;
};

class IndexGetOp: public Operator<CPUContext> {
 public:
  IndexGetOp(const OperatorDef& operator_def, Workspace* ws)
   : Operator(operator_def, ws) {}

  bool RunOnDevice() override {
    return DispatchHelper<IndexKeyTypes>::call(this, Input(1));
  }
  template <typename T>
  bool DoRunWithType() {
    auto& base = OperatorBase::Input<std::unique_ptr<IndexBase>>(0);
    auto* dict = dynamic_cast_if_rtti<Index<T>*>(base.get());
    CAFFE_ENFORCE(dict, "Wrong dictionary type given input keys.");
    const auto& keys = Input(1);
    auto* values = Output(0);
    values->ResizeLike(keys);
    return dict->Get(
        keys.data<T>(), values->mutable_data<TIndexValue>(), keys.size());
  }
};

class IndexLoadOp: public Operator<CPUContext> {
 public:
  IndexLoadOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator(operator_def, ws),
        skipFirstEntry_(
            OperatorBase::GetSingleArgument<int>("skip_first_entry", 0)) {}

  bool RunOnDevice() override {
    return DispatchHelper<IndexKeyTypes>::call(this, Input(1));
  }
  template <typename T>
  bool DoRunWithType() {
    auto& base = OperatorBase::Input<std::unique_ptr<IndexBase>>(0);
    auto* dict = dynamic_cast_if_rtti<Index<T>*>(base.get());
    CAFFE_ENFORCE(dict, "Wrong dictionary type given input keys.");
    const auto& keys = Input(1);
    const auto* keys_data = keys.data<T>();
    auto keys_size = keys.size();
    if (skipFirstEntry_) {
      CAFFE_ENFORCE(keys.size() > 0);
      ++keys_data;
      --keys_size;
    }
    return dict->Load(keys_data, keys_size);
  }

 private:
  bool skipFirstEntry_;
};

class IndexStoreOp: public Operator<CPUContext> {
 public:
  IndexStoreOp(const OperatorDef& operator_def, Workspace* ws)
   : Operator(operator_def, ws) {}

  bool RunOnDevice() override {
    auto& base = OperatorBase::Input<std::unique_ptr<IndexBase>>(0);
    return DispatchHelper<IndexKeyTypes>::call(this, base->Type());
  }

  template <typename T>
  bool DoRunWithType() {
    auto& base = OperatorBase::Input<std::unique_ptr<IndexBase>>(0);
    auto* dict = dynamic_cast_if_rtti<Index<T>*>(base.get());
    CHECK(dict);
    return dict->Store(Output(0));
  }
};

class IndexFreezeOp: public Operator<CPUContext> {
 public:
  IndexFreezeOp(const OperatorDef& operator_def, Workspace* ws)
   : Operator(operator_def, ws) {}

  bool RunOnDevice() override {
    auto& base = OperatorBase::Input<std::unique_ptr<IndexBase>>(0);
    base->Freeze();
    return true;
  }
};

class IndexSizeOp : public Operator<CPUContext> {
 public:
  IndexSizeOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator(operator_def, ws) {}

  bool RunOnDevice() override {
    auto& base = OperatorBase::Input<std::unique_ptr<IndexBase>>(0);
    auto* out = Output(0);
    out->Resize(std::vector<TIndex>{});
    *out->mutable_data<TIndexValue>() = base->Size();
    return true;
  }
};

REGISTER_CPU_OPERATOR(IntIndexCreate, IndexCreateOp<int32_t>);
REGISTER_CPU_OPERATOR(LongIndexCreate, IndexCreateOp<int64_t>);
REGISTER_CPU_OPERATOR(StringIndexCreate, IndexCreateOp<std::string>);

REGISTER_CPU_OPERATOR(IndexGet, IndexGetOp);
REGISTER_CPU_OPERATOR(IndexLoad, IndexLoadOp);
REGISTER_CPU_OPERATOR(IndexStore, IndexStoreOp);
REGISTER_CPU_OPERATOR(IndexFreeze, IndexFreezeOp);
REGISTER_CPU_OPERATOR(IndexSize, IndexSizeOp);

OPERATOR_SCHEMA(IntIndexCreate)
  .NumInputs(0)
  .NumOutputs(1)
  .SetDoc(R"DOC(
Creates a dictionary that maps int32 keys to consecutive integers
from 1 to max_elements. Zero is reserved for unknown keys.
)DOC")
  .Arg("max_elements", "Max number of elements, including the zero entry.")
  .Output(0, "handler", "Pointer to an Index instance.");

OPERATOR_SCHEMA(LongIndexCreate)
  .NumInputs(0)
  .NumOutputs(1)
  .SetDoc(R"DOC(
Creates a dictionary that maps int64 keys to consecutive integers
from 1 to max_elements. Zero is reserved for unknown keys.
)DOC")
  .Arg("max_elements", "Max number of elements, including the zero entry.")
  .Output(0, "handler", "Pointer to an Index instance.");

OPERATOR_SCHEMA(StringIndexCreate)
  .NumInputs(0)
  .NumOutputs(1)
  .SetDoc(R"DOC(
Creates a dictionary that maps string keys to consecutive integers
from 1 to max_elements. Zero is reserved for unknown keys.
)DOC")
  .Arg("max_elements", "Max number of elements, including the zero entry.")
  .Output(0, "handle", "Pointer to an Index instance.");

OPERATOR_SCHEMA(IndexGet)
  .NumInputs(2)
  .NumOutputs(1)
  .SetDoc(R"DOC(
Given an index handle and a tensor of keys, return an Int tensor of same shape
containing the indices for each of the keys. If the index is frozen, unknown
entries are given index 0. Otherwise, new entries are added into the index.
If an insert is necessary but max_elements has been reached, fail.
)DOC")
  .Input(0, "handle", "Pointer to an Index instance.")
  .Input(1, "keys", "Tensor of keys to be looked up.")
  .Output(0, "indices", "Indices for each of the keys.");

OPERATOR_SCHEMA(IndexFreeze)
  .NumInputs(1)
  .NumOutputs(0)
  .SetDoc(R"DOC(
Freezes the given index, disallowing creation of new index entries.
Should not be called concurrently with IndexGet.
)DOC")
  .Input(0, "handle", "Pointer to an Index instance.");

OPERATOR_SCHEMA(IndexLoad)
    .NumInputs(2)
    .NumOutputs(0)
    .SetDoc(R"DOC(
Loads the index from the given 1-D tensor. Elements in the tensor will be given
consecutive indexes starting at 1. Fails if tensor contains repeated elements.
)DOC")
    .Input(0, "handle", "Pointer to an Index instance.")
    .Input(1, "items", "1-D tensor with elements starting with index 1.")
    .Arg(
        "skip_first_entry",
        "If set, skips the first entry of the tensor. This allows "
        "to load tensors that are aligned with an embedding, where the first "
        "entry corresponds to the default 0 index entry.");

OPERATOR_SCHEMA(IndexStore)
  .NumInputs(1)
  .NumOutputs(1)
  .SetDoc(R"DOC(
Stores the keys of this index in a 1-D tensor. Since element 0 is reserved
for unknowns, the first element of the output tensor will be element of index 1.
)DOC")
  .Input(0, "handle", "Pointer to an Index instance.")
  .Output(0, "items", "1-D tensor with elements starting with index 1.");

OPERATOR_SCHEMA(IndexSize)
    .NumInputs(1)
    .NumOutputs(1)
    .SetDoc(R"DOC(
Returns the number of entries currently present in the index.
)DOC")
    .Input(0, "handle", "Pointer to an Index instance.")
    .Output(0, "items", "Scalar int64 tensor with number of entries.");

NO_GRADIENT(IndexGetOp);
NO_GRADIENT(IntIndexCreate);
NO_GRADIENT(LongIndexCreate);
NO_GRADIENT(StringIndexCreate);
SHOULD_NOT_DO_GRADIENT(IndexFreeze);
SHOULD_NOT_DO_GRADIENT(IndexLoad);
SHOULD_NOT_DO_GRADIENT(IndexStore);
SHOULD_NOT_DO_GRADIENT(IndexSize);
}  // namespace caffe2