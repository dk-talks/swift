// RUN: %target-swift-emit-silgen %s | %FileCheck %s

public struct Projection<T> {
  public var wrappedValue: T
}

@propertyWrapper
public struct Wrapper<T> {
  public var wrappedValue: T

  // CHECK-LABEL: sil [ossa] @$s26property_wrapper_parameter7WrapperV12wrappedValueACyxGx_tcfC : $@convention(method) <T> (@in T, @thin Wrapper<T>.Type) -> @out Wrapper<T>
  public init(wrappedValue: T) {
    self.wrappedValue = wrappedValue
  }

  public var projectedValue: Projection<T> {
    Projection(wrappedValue: wrappedValue)
  }

  // CHECK-LABEL: sil [ossa] @$s26property_wrapper_parameter7WrapperV14projectedValueACyxGAA10ProjectionVyxG_tcfC : $@convention(method) <T> (@in Projection<T>, @thin Wrapper<T>.Type) -> @out Wrapper<T>
  public init(projectedValue: Projection<T>) {
    self.wrappedValue = projectedValue.wrappedValue
  }
}

// CHECK-LABEL: sil [ossa] @$s26property_wrapper_parameter26testSimpleWrapperParameter5valueyAA0F0VySiG_tF : $@convention(thin) (Wrapper<Int>) -> ()
public func testSimpleWrapperParameter(@Wrapper value: Int) {
  _ = value
  _ = _value
  _ = $value

  // getter of $value #1 in testSimpleWrapperParameter(value:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter26testSimpleWrapperParameter5valueyAA0F0VySiG_tF6$valueL_AA10ProjectionVySiGvg : $@convention(thin) (Wrapper<Int>) -> Projection<Int>

  // getter of value #1 in testSimpleWrapperParameter(value:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter26testSimpleWrapperParameter5valueyAA0F0VySiG_tFACL_Sivg : $@convention(thin) (Wrapper<Int>) -> Int
}

// CHECK-LABEL: sil hidden [ossa] @$s26property_wrapper_parameter28simpleWrapperParameterCaller10projectionyAA10ProjectionVySiG_tF : $@convention(thin) (Projection<Int>) -> ()
func simpleWrapperParameterCaller(projection: Projection<Int>) {
  testSimpleWrapperParameter(value: projection.wrappedValue)
  // CHECK: function_ref @$s26property_wrapper_parameter7WrapperV12wrappedValueACyxGx_tcfC : $@convention(method) <τ_0_0> (@in τ_0_0, @thin Wrapper<τ_0_0>.Type) -> @out Wrapper<τ_0_0>

  testSimpleWrapperParameter($value: projection)
  // CHECK: function_ref @$s26property_wrapper_parameter7WrapperV14projectedValueACyxGAA10ProjectionVyxG_tcfC : $@convention(method) <τ_0_0> (@in Projection<τ_0_0>, @thin Wrapper<τ_0_0>.Type) -> @out Wrapper<τ_0_0>
}

// CHECK-LABEL: sil hidden [ossa] @$s26property_wrapper_parameter33testSimpleClosureWrapperParameteryyF : $@convention(thin) () -> ()
func testSimpleClosureWrapperParameter() {
  let closure: (Int) -> Void = { (@Wrapper value) in
    _ = value
    _ = _value
    _ = $value
  }

  closure(10)

  // implicit closure #1 in testSimpleClosureWrapperParameter()
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter33testSimpleClosureWrapperParameteryyFySicfu_ : $@convention(thin) (Int) -> ()

  // closure #1 in implicit closure #1 in testSimpleClosureWrapperParameter()
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter33testSimpleClosureWrapperParameteryyFySicfu_yAA0G0VySiGcfU_ : $@convention(thin) (Wrapper<Int>) -> ()

  // getter of $value #1 in closure #1 in implicit closure #1 in testSimpleClosureWrapperParameter()
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter33testSimpleClosureWrapperParameteryyFySicfu_yAA0G0VySiGcfU_6$valueL_AA10ProjectionVySiGvg : $@convention(thin) (Wrapper<Int>) -> Projection<Int>

  // getter of value #1 in closure #1 in implicit closure #1 in testSimpleClosureWrapperParameter()
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter33testSimpleClosureWrapperParameteryyFySicfu_yAA0G0VySiGcfU_5valueL_Sivg : $@convention(thin) (Wrapper<Int>) -> Int
}

@propertyWrapper
struct NonMutatingSetterWrapper<Value> {
  private var value: Value

  var wrappedValue: Value {
    get { value }
    nonmutating set { }
  }

  init(wrappedValue: Value) {
    self.value = wrappedValue
  }
}

@propertyWrapper
class ClassWrapper<Value> {
  var wrappedValue: Value

  init(wrappedValue: Value) {
    self.wrappedValue = wrappedValue
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s26property_wrapper_parameter21testNonMutatingSetter6value16value2yAA0efG7WrapperVySSG_AA05ClassJ0CySiGtF : $@convention(thin) (@guaranteed NonMutatingSetterWrapper<String>, @guaranteed ClassWrapper<Int>) -> ()
func testNonMutatingSetter(@NonMutatingSetterWrapper value1: String, @ClassWrapper value2: Int) {
  _ = value1
  value1 = "hello!"

  // getter of value1 #1 in testNonMutatingSetter(value1:value2:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter21testNonMutatingSetter6value16value2yAA0efG7WrapperVySSG_AA05ClassJ0CySiGtFACL_SSvg : $@convention(thin) (@guaranteed NonMutatingSetterWrapper<String>) -> @owned String

  // setter of value1 #1 in testNonMutatingSetter(value1:value2:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter21testNonMutatingSetter6value16value2yAA0efG7WrapperVySSG_AA05ClassJ0CySiGtFACL_SSvs : $@convention(thin) (@owned String, @guaranteed NonMutatingSetterWrapper<String>) -> ()

  _ = value2
  value2 = 10

  // getter of value2 #1 in testNonMutatingSetter(value1:value2:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter21testNonMutatingSetter6value16value2yAA0efG7WrapperVySSG_AA05ClassJ0CySiGtFADL_Sivg : $@convention(thin) (@guaranteed ClassWrapper<Int>) -> Int

  // setter of value2 #1 in testNonMutatingSetter(value1:value2:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter21testNonMutatingSetter6value16value2yAA0efG7WrapperVySSG_AA05ClassJ0CySiGtFADL_Sivs : $@convention(thin) (Int, @guaranteed ClassWrapper<Int>) -> ()
}

@propertyWrapper
struct ProjectionWrapper<Value> {
  var wrappedValue: Value

  var projectedValue: ProjectionWrapper<Value> { self }

  init(wrappedValue: Value) { self.wrappedValue = wrappedValue }

  init(projectedValue: ProjectionWrapper<Value>) {
    self.wrappedValue = projectedValue.wrappedValue
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s26property_wrapper_parameter27testImplicitPropertyWrapper10projectionyAA010ProjectionG0VySiG_tF : $@convention(thin) (ProjectionWrapper<Int>) -> ()
func testImplicitPropertyWrapper(projection: ProjectionWrapper<Int>) {
  let multiStatement: (ProjectionWrapper<Int>) -> Void = { $value in
    _ = value
    _ = _value
    _ = $value
  }

  multiStatement(projection)

  // implicit closure #1 in testImplicitPropertyWrapper(projection:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter27testImplicitPropertyWrapper10projectionyAA010ProjectionG0VySiG_tFyAFcfu_ : $@convention(thin) (ProjectionWrapper<Int>) -> ()

  // closure #1 in implicit closure #1 in testImplicitPropertyWrapper(projection:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter27testImplicitPropertyWrapper10projectionyAA010ProjectionG0VySiG_tFyAFcfu_yAFcfU_ : $@convention(thin) (ProjectionWrapper<Int>) -> ()

  // getter of $value #1 in closure #1 in implicit closure #1 in testImplicitPropertyWrapper(projection:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter27testImplicitPropertyWrapper10projectionyAA010ProjectionG0VySiG_tFyAFcfu_yAFcfU_6$valueL_AFvg : $@convention(thin) (ProjectionWrapper<Int>) -> ProjectionWrapper<Int>

  // getter of value #1 in closure #1 in implicit closure #1 in testImplicitPropertyWrapper(projection:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter27testImplicitPropertyWrapper10projectionyAA010ProjectionG0VySiG_tFyAFcfu_yAFcfU_5valueL_Sivg : $@convention(thin) () -> Int

  let _: (ProjectionWrapper<Int>) -> (Int, ProjectionWrapper<Int>) = { $value in
    (value, $value)
  }

  // implicit closure #2 in testImplicitPropertyWrapper(projection:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter27testImplicitPropertyWrapper10projectionyAA010ProjectionG0VySiG_tFSi_AFtAFcfu0_ : $@convention(thin) (ProjectionWrapper<Int>) -> (Int, ProjectionWrapper<Int>)

  // closure #2 in implicit closure #2 in testImplicitPropertyWrapper(projection:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter27testImplicitPropertyWrapper10projectionyAA010ProjectionG0VySiG_tFSi_AFtAFcfu0_Si_AFtAFcfU0_ : $@convention(thin) (ProjectionWrapper<Int>) -> (Int, ProjectionWrapper<Int>)

  // getter of $value #1 in closure #2 in implicit closure #2 in testImplicitPropertyWrapper(projection:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter27testImplicitPropertyWrapper10projectionyAA010ProjectionG0VySiG_tFSi_AFtAFcfu0_Si_AFtAFcfU0_6$valueL_AFvg : $@convention(thin) (ProjectionWrapper<Int>) -> ProjectionWrapper<Int>

  // getter of value #1 in closure #2 in implicit closure #2 in testImplicitPropertyWrapper(projection:)
  // CHECK: sil private [ossa] @$s26property_wrapper_parameter27testImplicitPropertyWrapper10projectionyAA010ProjectionG0VySiG_tFSi_AFtAFcfu0_Si_AFtAFcfU0_5valueL_Sivg : $@convention(thin) () -> Int
}
