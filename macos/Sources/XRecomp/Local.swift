import SwiftUI

/// View-local state without `@State`: in the Command Line Tools' SDK `@State` is a macro whose plugin
/// (SwiftUIMacros) ships only with Xcode, so it does not compile here. `@Local` keeps the value in a StateObject,
/// which is a plain property wrapper, with the same use: `@Local private var hover = false`, `$hover` a Binding.
@propertyWrapper
struct Local<T>: DynamicProperty {
    final class Box: ObservableObject {
        @Published var value: T
        init(_ v: T) { value = v }
    }

    @StateObject private var box: Box

    init(wrappedValue: T) {
        _box = StateObject(wrappedValue: Box(wrappedValue))
    }

    var wrappedValue: T {
        get { box.value }
        nonmutating set { box.value = newValue }
    }

    var projectedValue: Binding<T> { $box.value }
}
