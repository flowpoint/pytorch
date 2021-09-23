#include "lazy_tensor_core/csrc/ir.h"

#include <functional>
#include <sstream>

#include "lazy_tensors/computation_client/cache.h"
#include "lazy_tensors/computation_client/debug_macros.h"
#include "lazy_tensors/computation_client/sys_util.h"
#include "lazy_tensors/computation_client/util.h"
#include "lazy_tensors/str_cat.h"

namespace torch_lazy_tensors {
namespace ir {
namespace {

using ShapeCache =
    lazy_tensors::util::Cache<lazy_tensors::hash_t, lazy_tensors::Shape,
                              lazy_tensors::util::HashReducer>;

struct ScopeEntry {
  std::string name;
  size_t saved_next_id = 1;
};

struct ScopeContext {
  std::vector<ScopeEntry> scopes;
  size_t next_id = 1;
};

thread_local ScopeContext g_scope_context;

void PushScope(const std::string& name) {
  size_t id = g_scope_context.next_id;
  g_scope_context.scopes.push_back(
      {lazy_tensors::StrCat(name, ".", id), g_scope_context.next_id + 1});
  g_scope_context.next_id = 1;
}

void PopScope() {
  LTC_CHECK(!g_scope_context.scopes.empty());
  g_scope_context.next_id = g_scope_context.scopes.back().saved_next_id;
  g_scope_context.scopes.pop_back();
}

void ResetScopeContext() {
  LTC_CHECK_EQ(g_scope_context.scopes.size(), 0);
  g_scope_context.next_id = 1;
}

std::string GetCurrentScope() {
  std::string scope;
  for (auto& scope_entry : g_scope_context.scopes) {
    if (scope.empty()) {
      lazy_tensors::StrAppend(&scope, scope_entry.name);
    } else {
      lazy_tensors::StrAppend(&scope, "/", scope_entry.name);
    }
  }
  return scope;
}

void EmitShortFrameInfo(std::ostream& stream,
                        const std::vector<SourceLocation>& frames) {
  if (!frames.empty()) {
    const SourceLocation& frame = frames.front();
    std::string::size_type pos = frame.file.find_last_of('/');
    if (pos == std::string::npos) {
      pos = 0;
    } else {
      ++pos;
    }
    stream << ", location=" << frame.function << "@" << frame.file.substr(pos)
           << ":" << frame.line;
  }
}

}  // namespace

bool Use::operator<(const Use& rhs) const {
  if (node->op() != rhs.node->op()) {
    return node->op() < rhs.node->op();
  }
  if (operand_index != rhs.operand_index) {
    return operand_index < rhs.operand_index;
  }
  return index < rhs.index;
}

std::string Use::ToString() const {
  std::stringstream ss;
  ss << node->ToString() << ", operand_index=" << operand_index
     << ", index=" << index;
  return ss.str();
}

size_t Output::Hasher::operator()(const Output& output) const {
  return lazy_tensors::util::StdHashCombine(
      reinterpret_cast<std::ptrdiff_t>(output.node), output.index);
}

const lazy_tensors::Shape Output::shape() const {
  return lazy_tensors::Shape(node->aten_type(), node->aten_shape(index));
}

const lazy_tensors::Shape Output::node_shape() const {
  return lazy_tensors::Shape(node->aten_type(), node->aten_shape());
}
lazy_tensors::hash_t Output::hash() const {
  return lazy_tensors::util::HashCombine(node->hash(), index);
}

std::string Output::ToString() const {
  std::stringstream ss;
  ss << node->ToString() << ", index=" << index;
  return ss.str();
}

const lazy_tensors::Shape Value::shape() const {
  return lazy_tensors::Shape(node->aten_type(), node->aten_shape(index));
}
const lazy_tensors::Shape Value::node_shape() const {
  return lazy_tensors::Shape(node->aten_type(), node->aten_shape());
}
lazy_tensors::hash_t Value::hash() const {
  return lazy_tensors::util::HashCombine(node->hash(), index);
}

OpKind OpKind::Get(const std::string& name) {
  return OpKind(c10::Symbol::fromQualString(name));
}

lazy_tensors::hash_t OpKind::hash() const {
  return lazy_tensors::util::StringHash(op.toQualString());
}

Node::Node(OpKind op, OpList operands, std::vector<int64_t> aten_shape,
           c10::ScalarType aten_type, size_t num_outputs,
           lazy_tensors::hash_t hash_seed)
    : op_(std::move(op)),
      num_outputs_(num_outputs),
      aten_shape_(std::move(aten_shape)),
      aten_type_(std::move(aten_type)),
      node_hash_(lazy_tensors::util::HashCombine(op_.hash(), hash_seed)),
      hash_(node_hash_) {
  metadata_.scope = GetCurrentScope();
  metadata_.frame_info = GetFrameInfo();
  for (auto& operand : operands) {
    AddOperand(operand.node, operand.index);
    hash_ = lazy_tensors::util::HashCombine(hash_, operand.hash());
  }
}

Node::Node(OpKind op, OpList operands,
           std::vector<AtenShape> multi_out_aten_shape,
           c10::ScalarType aten_type, size_t num_outputs,
           lazy_tensors::hash_t hash_seed)
    : op_(std::move(op)),
      num_outputs_(num_outputs),
      multi_out_aten_shape_(std::move(multi_out_aten_shape)),
      aten_type_(std::move(aten_type)),
      // TODO(whc) missing hash contributition for multi shape
      node_hash_(lazy_tensors::util::HashCombine(op_.hash(), hash_seed)),
      hash_(node_hash_) {
  metadata_.scope = GetCurrentScope();
  metadata_.frame_info = GetFrameInfo();
  for (auto& operand : operands) {
    AddOperand(operand.node, operand.index);
    hash_ = lazy_tensors::util::HashCombine(hash_, operand.hash());
  }
}

Node::Node(OpKind op, OpList operands, size_t num_outputs,
           lazy_tensors::hash_t hash_seed)
    : op_(std::move(op)),
      num_outputs_(num_outputs),
      node_hash_(lazy_tensors::util::HashCombine(op_.hash(), hash_seed)),
      hash_(node_hash_) {
  metadata_.scope = GetCurrentScope();
  metadata_.frame_info = GetFrameInfo();
  for (auto& operand : operands) {
    AddOperand(operand.node, operand.index);
    hash_ = lazy_tensors::util::HashCombine(hash_, operand.hash());
  }
}

Node::Node(OpKind op, std::vector<int64_t> aten_shape,
           c10::ScalarType aten_type, size_t num_outputs,
           lazy_tensors::hash_t hash_seed)
    : op_(std::move(op)),
      num_outputs_(num_outputs),
      aten_shape_(std::move(aten_shape)),
      aten_type_(std::move(aten_type)),
      node_hash_(GetOpHash(op_, aten_shape_, aten_type_, hash_seed)),
      hash_(node_hash_) {
  metadata_.scope = GetCurrentScope();
  metadata_.frame_info = GetFrameInfo();
}
Node::Node(OpKind op, size_t num_outputs, lazy_tensors::hash_t hash_seed)
    : op_(std::move(op)),
      num_outputs_(num_outputs),
      // TODO(whc) trying to adapt Node base to TsNode that takes ::Shape
      // and (ab)uses SetShapeDeferred to convet it into aten shape.
      // what should be done for the hash in these cases?
      // what is done for other nodes that have deferred shape setting?
      node_hash_(lazy_tensors::util::HashCombine(op_.hash(), hash_seed)),
      hash_(node_hash_) {}
Node::~Node() {
  for (size_t i = 0; i < operands_as_outputs_.size(); ++i) {
    operands_[i]->RemoveUse(Use(this, i, operands_as_outputs_[i].index));
  }
}

const std::vector<int64_t>& Node::aten_shape(size_t output_index) const {
  if (num_outputs_ > 1) {
    return multi_out_aten_shape_.at(output_index);
  }
  LTC_CHECK_EQ(output_index, 0);
  return aten_shape_;
}

void Node::AddOperand(NodePtr node, size_t index) {
  LTC_CHECK_LT(index, node->num_outputs());
  operands_.push_back(std::move(node));
  operands_as_outputs_.push_back(Output(operands_.back().get(), index));
  operands_.back()->AddUse(Use(this, operands_.size() - 1, index));
}

void Node::ReplaceOperand(size_t operand_no, NodePtr node, size_t index) {
  LTC_CHECK_LT(index, node->num_outputs());
  Output* output = &operands_as_outputs_.at(operand_no);
  operands_[operand_no]->RemoveUse(Use(this, operand_no, output->index));
  node->AddUse(Use(this, operand_no, index));
  *output = Output(node.get(), index);
  operands_[operand_no] = std::move(node);
}

void Node::ReplaceAllUsesWith(NodePtr node, size_t index) {
  // A call to ReplaceOperand() will end up calling RemoveUse() into the
  // current node, so snapshot the current uses and iterate over them.
  std::vector<Use> current_uses(uses_.begin(), uses_.end());
  for (auto& use : current_uses) {
    use.node->ReplaceOperand(use.operand_index, node, index);
  }
}

std::string Node::ToString() const {
  std::stringstream ss;
  // TODO(whc) refactor away from lazy_tensors::Shape
  auto shape = lazy_tensors::Shape(aten_type_, aten_shape_);
  ss << shape << " " << op();
  if (num_outputs() > 1) {
    ss << ", num_outputs=" << num_outputs();
  }
  if (!metadata_.scope.empty()) {
    ss << ", scope=" << metadata_.scope;
  }
  EmitShortFrameInfo(ss, metadata_.frame_info);
  return ss.str();
}

lazy_tensors::hash_t Node::GetOpHash(OpKind op, std::vector<int64_t> aten_shape,
                                     c10::ScalarType aten_type,
                                     lazy_tensors::hash_t hash_seed) {
  if (lazy_tensors::Shape::IsDynamicMode()) {
    lazy_tensors::hash_t h = lazy_tensors::util::HashCombine(
        op.hash(), lazy_tensors::util::Hash(aten_shape.size()));
    return lazy_tensors::util::HashCombine(h, hash_seed);
  }
  // TODO(whc) refactor away from lazy_tensors::Shape
  auto lazy_shape = lazy_tensors::Shape(aten_type, aten_shape);
  lazy_tensors::hash_t h = lazy_tensors::util::HashCombine(
      op.hash(), lazy_tensors::util::Hash(lazy_shape.ToString()));
  return lazy_tensors::util::HashCombine(h, hash_seed);
}

// lazy_tensors::Shape Node::GetOpShape(
//     const std::function<lazy_tensors::Shape()>& shape_fn) const {
//   ShapeCache* shape_cache = GetShapeCache();
//   auto shape = shape_cache->Get(hash());
//   if (shape == nullptr) {
//     shape = shape_cache->Add(hash(),
//                              std::make_shared<lazy_tensors::Shape>(shape_fn()));
//   }
//   return *shape;
// }

std::vector<SourceLocation> Node::GetFrameInfo() {
  // At the time of writing, retrieving Python frames costs from 1us up to
  // 20us. This per IR Node. Since it is not unreasonable to have a many
  // hundreds of IR Node, this can be a multi-millisecond cost, which is not
  // negligible.
  static bool wants_frames =
      lazy_tensors::sys_util::GetEnvBool("LTC_IR_DEBUG", false);
  return wants_frames ? GetPythonFrames() : std::vector<SourceLocation>();
}

ScopePusher::ScopePusher(const std::string& name) { PushScope(name); }

ScopePusher::~ScopePusher() { PopScope(); }

void ScopePusher::ResetScopes() { ResetScopeContext(); }

lazy_tensors::Shape AtenToLazyShapeHelper(const Node& node) {
  if (node.num_outputs() >= 1) {
    return lazy_tensors::Shape(node.aten_type(), node.aten_shape());
  } else {
    std::vector<lazy_tensors::Shape> tuple_shapes;
    tuple_shapes.reserve(node.multi_out_aten_shape().size());
    for (size_t i = 0; i < node.multi_out_aten_shape().size(); i++) {
      tuple_shapes.emplace_back(lazy_tensors::Shape(
          node.aten_type(), node.multi_out_aten_shape().at(i)));
    }
    return lazy_tensors::Shape(tuple_shapes);
  }
}

}  // namespace ir
}  // namespace torch_lazy_tensors
