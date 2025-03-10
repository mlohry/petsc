#ifndef PETSCVECCUPMIMPL_H
#define PETSCVECCUPMIMPL_H

#include <petsc/private/vecimpl.h>
#include <../src/vec/vec/impls/dvecimpl.h> // for Vec_Seq

#if PetscDefined(HAVE_NVSHMEM)
PETSC_INTERN PetscErrorCode PetscNvshmemInitializeCheck(void);
PETSC_INTERN PetscErrorCode PetscNvshmemMalloc(size_t, void **);
PETSC_INTERN PetscErrorCode PetscNvshmemCalloc(size_t, void **);
PETSC_INTERN PetscErrorCode PetscNvshmemFree_Private(void *);
  #define PetscNvshmemFree(ptr) ((ptr) && (PetscNvshmemFree_Private(ptr) || ((ptr) = PETSC_NULLPTR, 0)))
PETSC_INTERN PetscErrorCode PetscNvshmemSum(PetscInt, PetscScalar *, const PetscScalar *);
PETSC_INTERN PetscErrorCode PetscNvshmemMax(PetscInt, PetscReal *, const PetscReal *);
PETSC_INTERN PetscErrorCode VecNormAsync_NVSHMEM(Vec, NormType, PetscReal *);
PETSC_INTERN PetscErrorCode VecAllocateNVSHMEM_SeqCUDA(Vec);
#else
  #define PetscNvshmemFree(ptr) 0
#endif

#if defined(__cplusplus) && PetscDefined(HAVE_DEVICE)
  #include <petsc/private/deviceimpl.h>
  #include <petsc/private/cupmblasinterface.hpp>

  #include <petsc/private/cpp/functional.hpp>

  #include <limits>  // std::numeric_limits
  #include <cstring> // std::memset

namespace Petsc
{

namespace vec
{

namespace cupm
{

namespace impl
{

namespace
{

// ==========================================================================================
// UseCUPMHostAlloc_
//
// A simple RAII helper for PetscMallocSet[CUDA|HIP]Host(). it exists because integrating the
// regular versions would be an enormous pain to square with the templated types...
// ==========================================================================================
template <device::cupm::DeviceType T>
class UseCUPMHostAlloc_ : device::cupm::impl::Interface<T> {
public:
  PETSC_CUPM_INHERIT_INTERFACE_TYPEDEFS_USING(interface_type, T);

  UseCUPMHostAlloc_(bool) noexcept;
  ~UseCUPMHostAlloc_() noexcept;

  PETSC_NODISCARD bool value() const noexcept;

private:
    // would have loved to just do
    //
    // const auto oldmalloc = PetscTrMalloc;
    //
    // but in order to use auto the member needs to be static; in order to be static it must
    // also be constexpr -- which in turn requires an initializer (also implicitly required by
    // auto). But constexpr needs a constant expression initializer, so we can't initialize it
    // with global (mutable) variables...
  #define DECLTYPE_AUTO(left, right) decltype(right) left = right
  const DECLTYPE_AUTO(oldmalloc_, PetscTrMalloc);
  const DECLTYPE_AUTO(oldfree_, PetscTrFree);
  const DECLTYPE_AUTO(oldrealloc_, PetscTrRealloc);
  #undef DECLTYPE_AUTO
  bool v_;
};

template <device::cupm::DeviceType T>
inline UseCUPMHostAlloc_<T>::UseCUPMHostAlloc_(bool useit) noexcept : v_(useit)
{
  PetscFunctionBegin;
  if (useit) {
    // all unused arguments are un-named, this saves having to add PETSC_UNUSED to them all
    PetscTrMalloc = [](std::size_t sz, PetscBool clear, int, const char *, const char *, void **ptr) {
      PetscFunctionBegin;
      PetscCallCUPM(cupmMallocHost(ptr, sz));
      if (clear) std::memset(*ptr, 0, sz);
      PetscFunctionReturn(0);
    };
    PetscTrFree = [](void *ptr, int, const char *, const char *) {
      PetscFunctionBegin;
      PetscCallCUPM(cupmFreeHost(ptr));
      PetscFunctionReturn(0);
    };
    PetscTrRealloc = [](std::size_t, int, const char *, const char *, void **) {
      // REVIEW ME: can be implemented by malloc->copy->free?
      SETERRQ(PETSC_COMM_SELF, PETSC_ERR_SUP, "%s has no realloc()", cupmName());
    };
  }
  PetscFunctionReturnVoid();
}

template <device::cupm::DeviceType T>
inline bool UseCUPMHostAlloc_<T>::value() const noexcept
{
  return v_;
}

template <device::cupm::DeviceType T>
inline UseCUPMHostAlloc_<T>::~UseCUPMHostAlloc_() noexcept
{
  PetscFunctionBegin;
  if (value()) {
    PetscTrMalloc  = oldmalloc_;
    PetscTrFree    = oldfree_;
    PetscTrRealloc = oldrealloc_;
  }
  PetscFunctionReturnVoid();
}

struct no_op {
  template <typename... T>
  constexpr PetscErrorCode operator()(T &&...) const noexcept
  {
    return 0;
  }
};

template <typename T>
struct CooPair {
  using value_type = T;
  using size_type  = PetscCount;

  value_type *&device;
  value_type *&host;
  size_type    size;
};

template <typename U>
static constexpr CooPair<U> make_coo_pair(U *&device, U *&host, PetscCount size) noexcept
{
  return {device, host, size};
}

} // anonymous namespace

// forward declarations
template <device::cupm::DeviceType>
class VecSeq_CUPM;
template <device::cupm::DeviceType>
class VecMPI_CUPM;

// ==========================================================================================
// Vec_CUPMBase
//
// Base class for the VecSeq and VecMPI CUPM implementations. On top of the usual DeviceType
// template parameter it also uses CRTP to be able to use values/calls specific to either
// VecSeq or VecMPI. This is in effect "inside-out" polymorphism.
// ==========================================================================================
template <device::cupm::DeviceType T, typename Derived>
class Vec_CUPMBase : device::cupm::impl::BlasInterface<T> {
public:
  PETSC_CUPMBLAS_INHERIT_INTERFACE_TYPEDEFS_USING(cupmBlasInterface_t, T);
  // ==========================================================================================
  // Vec_CUPMBase::vector_array
  //
  // RAII versions of the get/restore array routines. Determines constness of the pointer type,
  // holds the pointer itself provides the implicit conversion operator
  // ==========================================================================================
  template <PetscMemType, PetscMemoryAccessMode>
  class vector_array;

private:
  // A debug check to ensure that a given pointer-memtype pairing taken from user-land is
  // actually correct. Errors on mismatch
  PETSC_NODISCARD static PetscErrorCode CheckPointerMatchesMemType_(const void *ptr, PetscMemType mtype) noexcept
  {
    PetscFunctionBegin;
    if (PetscDefined(USE_DEBUG) && ptr) {
      PetscMemType ptr_mtype;

      PetscCall(PetscCUPMGetMemType(ptr, &ptr_mtype));
      if (mtype == PETSC_MEMTYPE_HOST) {
        PetscCheck(PetscMemTypeHost(ptr_mtype), PETSC_COMM_SELF, PETSC_ERR_POINTER, "Pointer %p declared as %s does not match actual memtype %s", ptr, PetscMemTypeToString(mtype), PetscMemTypeToString(ptr_mtype));
      } else if (mtype == PETSC_MEMTYPE_DEVICE) {
        // generic "device" memory should only care if the actual memtype is also generically
        // "device"
        PetscCheck(PetscMemTypeDevice(ptr_mtype), PETSC_COMM_SELF, PETSC_ERR_POINTER, "Pointer %p declared as %s does not match actual memtype %s", ptr, PetscMemTypeToString(mtype), PetscMemTypeToString(ptr_mtype));
      } else {
        PetscCheck(mtype == ptr_mtype, PETSC_COMM_SELF, PETSC_ERR_POINTER, "Pointer %p declared as %s does not match actual memtype %s", ptr, PetscMemTypeToString(mtype), PetscMemTypeToString(ptr_mtype));
      }
    }
    PetscFunctionReturn(0);
  }

  // The final stop in the GetHandles_/GetFromHandles_ chain. This retrieves the various
  // compute handles and ensure the given PetscDeviceContext is of the right type
  PETSC_NODISCARD static PetscErrorCode GetFromHandleDispatch_(PetscDeviceContext, cupmBlasHandle_t *, cupmStream_t *) noexcept;
  PETSC_NODISCARD static PetscErrorCode GetHandleDispatch_(PetscDeviceContext *, cupmBlasHandle_t *, cupmStream_t *) noexcept;

protected:
  PETSC_NODISCARD static PetscErrorCode VecView_Debug(Vec v, const char *message = "") noexcept
  {
    const auto   pobj  = PetscObjectCast(v);
    const auto   vimpl = VecIMPLCast(v);
    const auto   vcu   = VecCUPMCast(v);
    PetscMemType mtype;
    MPI_Comm     comm;

    PetscFunctionBegin;
    PetscValidPointer(vimpl, 1);
    PetscValidPointer(vcu, 1);
    PetscCall(PetscObjectGetComm(pobj, &comm));
    PetscCall(PetscPrintf(comm, "---------- %s ----------\n", message));
    PetscCall(PetscObjectPrintClassNamePrefixType(pobj, PETSC_VIEWER_STDOUT_(comm)));
    PetscCall(PetscPrintf(comm, "Address:             %p\n", v));
    PetscCall(PetscPrintf(comm, "Size:                %" PetscInt_FMT "\n", v->map->n));
    PetscCall(PetscPrintf(comm, "Offload mask:        %s\n", PetscOffloadMaskToString(v->offloadmask)));
    PetscCall(PetscPrintf(comm, "Host ptr:            %p\n", vimpl->array));
    PetscCall(PetscPrintf(comm, "Device ptr:          %p\n", vcu->array_d));
    PetscCall(PetscPrintf(comm, "Device alloced ptr:  %p\n", vcu->array_allocated_d));
    PetscCall(PetscCUPMGetMemType(vcu->array_d, &mtype));
    PetscCall(PetscPrintf(comm, "dptr is device mem?  %s\n", PetscBools[static_cast<PetscBool>(PetscMemTypeDevice(mtype))]));
    PetscFunctionReturn(0);
  }

  // Helper routines to retrieve various combinations of handles. The first set (GetHandles_)
  // gets a PetscDeviceContext along with it, while the second set (GetHandlesFrom_) assumes
  // you've gotten the PetscDeviceContext already, and retrieves the handles from it. All of
  // them check that the PetscDeviceContext is of the appropriate type
  PETSC_NODISCARD static PetscErrorCode GetHandles_(PetscDeviceContext *, cupmBlasHandle_t * = nullptr, cupmStream_t * = nullptr) noexcept;
  PETSC_NODISCARD static PetscErrorCode GetHandles_(PetscDeviceContext *, cupmStream_t *) noexcept;
  PETSC_NODISCARD static PetscErrorCode GetHandles_(cupmStream_t *) noexcept;
  PETSC_NODISCARD static PetscErrorCode GetHandles_(cupmBlasHandle_t *) noexcept;

  PETSC_NODISCARD static PetscErrorCode GetHandlesFrom_(PetscDeviceContext, cupmBlasHandle_t *, cupmStream_t * = nullptr) noexcept;
  PETSC_NODISCARD static PetscErrorCode GetHandlesFrom_(PetscDeviceContext, cupmStream_t *) noexcept;

  // Delete the allocated device array if required and replace it with the given array
  PETSC_NODISCARD static PetscErrorCode ResetAllocatedDevicePtr_(PetscDeviceContext, Vec, PetscScalar * = nullptr) noexcept;
  // Check either the host or device impl pointer is allocated and allocate it if
  // isn't. CastFunctionType casts the Vec to the required type and returns the pointer
  template <typename CastFunctionType>
  PETSC_NODISCARD static PetscErrorCode VecAllocateCheck_(Vec, void *&, CastFunctionType &&) noexcept;
  // Check the CUPM part (v->spptr) is allocated, otherwise allocate it
  PETSC_NODISCARD static PetscErrorCode VecCUPMAllocateCheck_(Vec) noexcept;
  // Check the Host part (v->data) is allocated, otherwise allocate it
  PETSC_NODISCARD static PetscErrorCode VecIMPLAllocateCheck_(Vec) noexcept;
  // Check the Host array is allocated, otherwise allocate it
  PETSC_NODISCARD static PetscErrorCode HostAllocateCheck_(PetscDeviceContext, Vec) noexcept;
  // Check the CUPM array is allocated, otherwise allocate it
  PETSC_NODISCARD static PetscErrorCode DeviceAllocateCheck_(PetscDeviceContext, Vec) noexcept;
  // Copy HTOD, allocating device if necessary
  PETSC_NODISCARD static PetscErrorCode CopyToDevice_(PetscDeviceContext, Vec, bool = false) noexcept;
  // Copy DTOH, allocating host if necessary
  PETSC_NODISCARD static PetscErrorCode CopyToHost_(PetscDeviceContext, Vec, bool = false) noexcept;

public:
  struct Vec_CUPM {
    PetscScalar *array_d;           // gpu data
    PetscScalar *array_allocated_d; // does PETSc own the array ptr?
    PetscBool    nvshmem;           // is array allocated in nvshmem? It is used to allocate
                                    // Mvctx->lvec in nvshmem

    // COO stuff
    PetscCount *jmap1_d; // [m+1]: i-th entry of the vector has jmap1[i+1]-jmap1[i] repeats
                         // in COO arrays
    PetscCount *perm1_d; // [tot1]: permutation array for local entries
    PetscCount *imap2_d; // [nnz2]: i-th unique entry in recvbuf is imap2[i]-th entry in
                         // the vector
    PetscCount *jmap2_d; // [nnz2+1]
    PetscCount *perm2_d; // [recvlen]
    PetscCount *Cperm_d; // [sendlen]: permutation array to fill sendbuf[]. 'C' for
                         // communication

    // Buffers for remote values in VecSetValuesCOO()
    PetscScalar *sendbuf_d;
    PetscScalar *recvbuf_d;
  };

  // Cast the Vec to its Vec_CUPM struct, i.e. return the result of (Vec_CUPM *)v->spptr
  PETSC_NODISCARD static Vec_CUPM *VecCUPMCast(Vec) noexcept;
  // Cast the Vec to its host struct, i.e. return the result of (Vec_Seq *)v->data
  template <typename U = Derived>
  PETSC_NODISCARD static constexpr auto VecIMPLCast(Vec v) noexcept -> decltype(U::VecIMPLCast_(v));
  // Get the PetscLogEvents for HTOD and DTOH
  PETSC_NODISCARD static constexpr PetscLogEvent VEC_CUPMCopyToGPU() noexcept;
  PETSC_NODISCARD static constexpr PetscLogEvent VEC_CUPMCopyFromGPU() noexcept;
  // Get the VecTypes
  PETSC_NODISCARD static constexpr VecType VECSEQCUPM() noexcept;
  PETSC_NODISCARD static constexpr VecType VECMPICUPM() noexcept;
  // Get the VecType of the calling vector
  template <typename U = Derived>
  PETSC_NODISCARD static constexpr VecType         VECIMPLCUPM() noexcept;
  PETSC_NODISCARD static constexpr PetscRandomType PETSCDEVICERAND() noexcept;

  // Call the host destroy function, i.e. VecDestroy_Seq()
  PETSC_NODISCARD static PetscErrorCode VecDestroy_IMPL(Vec) noexcept;
  // Call the host reset function, i.e. VecResetArray_Seq()
  PETSC_NODISCARD static PetscErrorCode VecResetArray_IMPL(Vec) noexcept;
  // ... you get the idea
  PETSC_NODISCARD static PetscErrorCode VecPlaceArray_IMPL(Vec, const PetscScalar *) noexcept;
  // Call the host creation function, i.e. VecCreate_Seq(), and also initialize the CUPM part
  // along with it if needed
  PETSC_NODISCARD static PetscErrorCode VecCreate_IMPL_Private(Vec, PetscBool *, PetscInt = 0, PetscScalar * = nullptr) noexcept;

  // Shorthand for creating vector_array's. Need functions to create them, otherwise using them
  // as an unnamed temporary leads to most vexing parse
  PETSC_NODISCARD static auto DeviceArrayRead(PetscDeviceContext dctx, Vec v) noexcept PETSC_DECLTYPE_AUTO_RETURNS(vector_array<PETSC_MEMTYPE_DEVICE, PETSC_MEMORY_ACCESS_READ>{dctx, v});
  PETSC_NODISCARD static auto DeviceArrayWrite(PetscDeviceContext dctx, Vec v) noexcept PETSC_DECLTYPE_AUTO_RETURNS(vector_array<PETSC_MEMTYPE_DEVICE, PETSC_MEMORY_ACCESS_WRITE>{dctx, v});
  PETSC_NODISCARD static auto DeviceArrayReadWrite(PetscDeviceContext dctx, Vec v) noexcept PETSC_DECLTYPE_AUTO_RETURNS(vector_array<PETSC_MEMTYPE_DEVICE, PETSC_MEMORY_ACCESS_READ_WRITE>{dctx, v});
  PETSC_NODISCARD static auto HostArrayRead(PetscDeviceContext dctx, Vec v) noexcept PETSC_DECLTYPE_AUTO_RETURNS(vector_array<PETSC_MEMTYPE_HOST, PETSC_MEMORY_ACCESS_READ>{dctx, v});
  PETSC_NODISCARD static auto HostArrayWrite(PetscDeviceContext dctx, Vec v) noexcept PETSC_DECLTYPE_AUTO_RETURNS(vector_array<PETSC_MEMTYPE_HOST, PETSC_MEMORY_ACCESS_WRITE>{dctx, v});
  PETSC_NODISCARD static auto HostArrayReadWrite(PetscDeviceContext dctx, Vec v) noexcept PETSC_DECLTYPE_AUTO_RETURNS(vector_array<PETSC_MEMTYPE_HOST, PETSC_MEMORY_ACCESS_READ_WRITE>{dctx, v});

  // disallow implicit conversion
  template <typename U>
  PETSC_NODISCARD static UseCUPMHostAlloc_<T> UseCUPMHostAlloc(U) noexcept = delete;
  // utility for using cupmHostAlloc()
  PETSC_NODISCARD static UseCUPMHostAlloc_<T> UseCUPMHostAlloc(bool) noexcept;
  PETSC_NODISCARD static UseCUPMHostAlloc_<T> UseCUPMHostAlloc(PetscBool) noexcept;

  // ops-table functions
  PETSC_NODISCARD static PetscErrorCode create(Vec) noexcept;
  PETSC_NODISCARD static PetscErrorCode destroy(Vec) noexcept;
  template <PetscMemType, PetscMemoryAccessMode, bool = false>
  PETSC_NODISCARD static PetscErrorCode getarray(Vec, PetscScalar **, PetscDeviceContext) noexcept;
  template <PetscMemType, PetscMemoryAccessMode, bool = false>
  PETSC_NODISCARD static PetscErrorCode getarray(Vec, PetscScalar **) noexcept;
  template <PetscMemType, PetscMemoryAccessMode>
  PETSC_NODISCARD static PetscErrorCode restorearray(Vec, PetscScalar **, PetscDeviceContext) noexcept;
  template <PetscMemType, PetscMemoryAccessMode>
  PETSC_NODISCARD static PetscErrorCode restorearray(Vec, PetscScalar **) noexcept;
  template <PetscMemoryAccessMode>
  PETSC_NODISCARD static PetscErrorCode getarrayandmemtype(Vec, PetscScalar **, PetscMemType *, PetscDeviceContext) noexcept;
  template <PetscMemoryAccessMode>
  PETSC_NODISCARD static PetscErrorCode getarrayandmemtype(Vec, PetscScalar **, PetscMemType *) noexcept;
  template <PetscMemoryAccessMode>
  PETSC_NODISCARD static PetscErrorCode restorearrayandmemtype(Vec, PetscScalar **, PetscDeviceContext) noexcept;
  template <PetscMemoryAccessMode>
  PETSC_NODISCARD static PetscErrorCode restorearrayandmemtype(Vec, PetscScalar **) noexcept;
  template <PetscMemType>
  PETSC_NODISCARD static PetscErrorCode replacearray(Vec, const PetscScalar *) noexcept;
  template <PetscMemType>
  PETSC_NODISCARD static PetscErrorCode resetarray(Vec) noexcept;
  template <PetscMemType>
  PETSC_NODISCARD static PetscErrorCode placearray(Vec, const PetscScalar *) noexcept;

  // common ops shared between Seq and MPI
  PETSC_NODISCARD static PetscErrorCode Create_CUPM(Vec) noexcept;
  PETSC_NODISCARD static PetscErrorCode Create_CUPMBase(MPI_Comm, PetscInt, PetscInt, PetscInt, Vec *, PetscBool, PetscLayout /*reference*/ = nullptr) noexcept;
  PETSC_NODISCARD static PetscErrorCode Initialize_CUPMBase(Vec, PetscBool, PetscScalar *, PetscScalar *, PetscDeviceContext) noexcept;
  template <typename SetupFunctionT = no_op>
  PETSC_NODISCARD static PetscErrorCode Duplicate_CUPMBase(Vec, Vec *, PetscDeviceContext, SetupFunctionT && = SetupFunctionT{}) noexcept;
  PETSC_NODISCARD static PetscErrorCode BindToCPU_CUPMBase(Vec, PetscBool, PetscDeviceContext) noexcept;
  PETSC_NODISCARD static PetscErrorCode GetArrays_CUPMBase(Vec, const PetscScalar **, const PetscScalar **, PetscOffloadMask *, PetscDeviceContext) noexcept;
  PETSC_NODISCARD static PetscErrorCode ResetPreallocationCOO_CUPMBase(Vec, PetscDeviceContext) noexcept;
  template <std::size_t NCount = 0, std::size_t NScal = 0>
  PETSC_NODISCARD static PetscErrorCode SetPreallocationCOO_CUPMBase(Vec, PetscCount, const PetscInt[], PetscDeviceContext, const std::array<CooPair<PetscCount>, NCount> & = {}, const std::array<CooPair<PetscScalar>, NScal> & = {}) noexcept;
};

// ==========================================================================================
// Vec_CUPMBase::vector_array
//
// RAII versions of the get/restore array routines. Determines constness of the pointer type,
// holds the pointer itself and provides the implicit conversion operator.
//
// On construction this calls the moral equivalent of Vec[CUPM]GetArray[Read|Write]()
// (depending on PetscMemoryAccessMode) and on destruction automatically restores the array
// for you
// ==========================================================================================
template <device::cupm::DeviceType T, typename D>
template <PetscMemType MT, PetscMemoryAccessMode MA>
class Vec_CUPMBase<T, D>::vector_array {
public:
  static const auto memory_type = MT;
  static const auto access_type = MA;

  using value_type        = PetscScalar;
  using pointer_type      = value_type *;
  using cupm_pointer_type = cupmScalar_t *;

  vector_array(PetscDeviceContext, Vec) noexcept;
  ~vector_array() noexcept;

  constexpr vector_array(vector_array &&) noexcept            = default;
  constexpr vector_array &operator=(vector_array &&) noexcept = default;

  pointer_type      data() const noexcept;
  cupm_pointer_type cupmdata() const noexcept;

  operator pointer_type() const noexcept;
  // in case pointer_type == cupmscalar_pointer_type we don't want this overload to exist, so
  // we make a dummy template parameter to allow SFINAE to nix it for us
  template <typename U = pointer_type, typename = util::enable_if_t<!std::is_same<U, cupm_pointer_type>::value>>
  operator cupm_pointer_type() const noexcept;

private:
  pointer_type       ptr_  = nullptr;
  PetscDeviceContext dctx_ = nullptr;
  Vec                v_    = nullptr;
};

// ==========================================================================================
// Vec_CUPMBase::vector_array - Static Variables
// ==========================================================================================

template <device::cupm::DeviceType T, typename D>
template <PetscMemType MT, PetscMemoryAccessMode MA>
const PetscMemType Vec_CUPMBase<T, D>::vector_array<MT, MA>::memory_type;

template <device::cupm::DeviceType T, typename D>
template <PetscMemType MT, PetscMemoryAccessMode MA>
const PetscMemoryAccessMode Vec_CUPMBase<T, D>::vector_array<MT, MA>::access_type;

// ==========================================================================================
// Vec_CUPMBase::vector_array - Public API
// ==========================================================================================

template <device::cupm::DeviceType T, typename D>
template <PetscMemType MT, PetscMemoryAccessMode MA>
inline Vec_CUPMBase<T, D>::vector_array<MT, MA>::vector_array(PetscDeviceContext dctx, Vec v) noexcept : dctx_(dctx), v_(v)
{
  PetscFunctionBegin;
  PetscCallAbort(PETSC_COMM_SELF, getarray<MT, MA, true>(v, &ptr_, dctx));
  PetscFunctionReturnVoid();
}

template <device::cupm::DeviceType T, typename D>
template <PetscMemType MT, PetscMemoryAccessMode MA>
inline Vec_CUPMBase<T, D>::vector_array<MT, MA>::~vector_array() noexcept
{
  PetscFunctionBegin;
  PetscCallAbort(PETSC_COMM_SELF, restorearray<MT, MA>(v_, &ptr_, dctx_));
  PetscFunctionReturnVoid();
}

template <device::cupm::DeviceType T, typename D>
template <PetscMemType MT, PetscMemoryAccessMode MA>
inline typename Vec_CUPMBase<T, D>::template vector_array<MT, MA>::pointer_type Vec_CUPMBase<T, D>::vector_array<MT, MA>::data() const noexcept
{
  return ptr_;
}

template <device::cupm::DeviceType T, typename D>
template <PetscMemType MT, PetscMemoryAccessMode MA>
inline typename Vec_CUPMBase<T, D>::template vector_array<MT, MA>::cupm_pointer_type Vec_CUPMBase<T, D>::vector_array<MT, MA>::cupmdata() const noexcept
{
  return cupmScalarPtrCast(data());
}

template <device::cupm::DeviceType T, typename D>
template <PetscMemType MT, PetscMemoryAccessMode MA>
inline Vec_CUPMBase<T, D>::vector_array<MT, MA>::operator pointer_type() const noexcept
{
  return data();
}

// in case pointer_type == cupmscalar_pointer_type we don't want this overload to exist, so
// we make a dummy template parameter to allow SFINAE to nix it for us
template <device::cupm::DeviceType T, typename D>
template <PetscMemType MT, PetscMemoryAccessMode MA>
template <typename U, typename>
inline Vec_CUPMBase<T, D>::vector_array<MT, MA>::operator cupm_pointer_type() const noexcept
{
  return cupmdata();
}

// ==========================================================================================
// Vec_CUPMBase - Private API
// ==========================================================================================

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::GetFromHandleDispatch_(PetscDeviceContext dctx, cupmBlasHandle_t *handle, cupmStream_t *stream) noexcept
{
  PetscFunctionBegin;
  PetscValidDeviceContext(dctx, 1);
  if (handle) PetscValidPointer(handle, 2);
  if (stream) PetscValidPointer(stream, 3);
  if (PetscDefined(USE_DEBUG)) {
    PetscDeviceType dtype;

    PetscCall(PetscDeviceContextGetDeviceType(dctx, &dtype));
    PetscCheckCompatibleDeviceTypes(PETSC_DEVICE_CUPM(), -1, dtype, 1);
  }
  if (handle) PetscCall(PetscDeviceContextGetBLASHandle_Internal(dctx, handle));
  if (stream) PetscCall(PetscDeviceContextGetStreamHandle_Internal(dctx, stream));
  PetscFunctionReturn(0);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::GetHandleDispatch_(PetscDeviceContext *dctx, cupmBlasHandle_t *handle, cupmStream_t *stream) noexcept
{
  PetscDeviceContext dctx_loc = nullptr;

  PetscFunctionBegin;
  // silence uninitialized variable warnings
  if (dctx) *dctx = nullptr;
  PetscCall(PetscDeviceContextGetCurrentContext(&dctx_loc));
  PetscCall(GetFromHandleDispatch_(dctx_loc, handle, stream));
  if (dctx) *dctx = dctx_loc;
  PetscFunctionReturn(0);
}

// ==========================================================================================
// Vec_CUPMBase - Protected API
// ==========================================================================================

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::GetHandles_(PetscDeviceContext *dctx, cupmBlasHandle_t *handle, cupmStream_t *stream) noexcept
{
  return GetHandleDispatch_(dctx, handle, stream);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::GetHandles_(PetscDeviceContext *dctx, cupmStream_t *stream) noexcept
{
  return GetHandles_(dctx, nullptr, stream);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::GetHandles_(cupmStream_t *stream) noexcept
{
  return GetHandles_(nullptr, stream);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::GetHandles_(cupmBlasHandle_t *handle) noexcept
{
  return GetHandles_(nullptr, handle);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::GetHandlesFrom_(PetscDeviceContext dctx, cupmBlasHandle_t *handle, cupmStream_t *stream) noexcept
{
  return GetFromHandleDispatch_(dctx, handle, stream);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::GetHandlesFrom_(PetscDeviceContext dctx, cupmStream_t *stream) noexcept
{
  return GetHandlesFrom_(dctx, nullptr, stream);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::ResetAllocatedDevicePtr_(PetscDeviceContext dctx, Vec v, PetscScalar *new_value) noexcept
{
  auto &device_array = VecCUPMCast(v)->array_allocated_d;

  PetscFunctionBegin;
  if (device_array) {
    if (PetscDefined(HAVE_NVSHMEM) && VecCUPMCast(v)->nvshmem) {
      PetscCall(PetscNvshmemFree(device_array));
    } else {
      cupmStream_t stream;

      PetscCall(GetHandlesFrom_(dctx, &stream));
      PetscCallCUPM(cupmFreeAsync(device_array, stream));
    }
  }
  device_array = new_value;
  PetscFunctionReturn(0);
}

namespace
{

PETSC_NODISCARD inline PetscErrorCode VecCUPMCheckMinimumPinnedMemory_Internal(Vec v) noexcept
{
  auto      mem = static_cast<PetscInt>(v->minimum_bytes_pinned_memory);
  PetscBool flg;

  PetscFunctionBegin;
  PetscObjectOptionsBegin(PetscObjectCast(v));
  PetscCall(PetscOptionsRangeInt("-vec_pinned_memory_min", "Minimum size (in bytes) for an allocation to use pinned memory on host", "VecSetPinnedMemoryMin", mem, &mem, &flg, 0, std::numeric_limits<decltype(mem)>::max()));
  if (flg) v->minimum_bytes_pinned_memory = mem;
  PetscOptionsEnd();
  PetscFunctionReturn(0);
}

} // anonymous namespace

template <device::cupm::DeviceType T, typename D>
template <typename CastFunctionType>
inline PetscErrorCode Vec_CUPMBase<T, D>::VecAllocateCheck_(Vec v, void *&dest, CastFunctionType &&cast) noexcept
{
  PetscFunctionBegin;
  if (PetscLikely(dest)) PetscFunctionReturn(0);
  // do the check here so we don't have to do it in every function
  PetscCall(checkCupmBlasIntCast(v->map->n));
  {
    auto impl = cast(v);

    PetscCall(PetscNew(&impl));
    dest = impl;
  }
  PetscFunctionReturn(0);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::VecIMPLAllocateCheck_(Vec v) noexcept
{
  PetscFunctionBegin;
  PetscCall(VecAllocateCheck_(v, v->data, VecIMPLCast<D>));
  PetscFunctionReturn(0);
}

// allocate the Vec_CUPM struct. this is normally done through DeviceAllocateCheck_(), but in
// certain circumstances (such as when the user places the device array) we do not want to do
// the full DeviceAllocateCheck_() as it also allocates the array
template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::VecCUPMAllocateCheck_(Vec v) noexcept
{
  PetscFunctionBegin;
  PetscCall(VecAllocateCheck_(v, v->spptr, VecCUPMCast));
  PetscFunctionReturn(0);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::HostAllocateCheck_(PetscDeviceContext, Vec v) noexcept
{
  PetscFunctionBegin;
  PetscCall(VecIMPLAllocateCheck_(v));
  if (auto &alloc = VecIMPLCast(v)->array_allocated) PetscFunctionReturn(0);
  else {
    PetscCall(VecCUPMCheckMinimumPinnedMemory_Internal(v));
    {
      const auto n     = v->map->n;
      const auto useit = UseCUPMHostAlloc((n * sizeof(*alloc)) > v->minimum_bytes_pinned_memory);

      v->pinned_memory = static_cast<decltype(v->pinned_memory)>(useit.value());
      PetscCall(PetscMalloc1(n, &alloc));
    }
    if (!VecIMPLCast(v)->array) VecIMPLCast(v)->array = alloc;
    if (v->offloadmask == PETSC_OFFLOAD_UNALLOCATED) v->offloadmask = PETSC_OFFLOAD_CPU;
  }
  PetscFunctionReturn(0);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::DeviceAllocateCheck_(PetscDeviceContext dctx, Vec v) noexcept
{
  PetscFunctionBegin;
  PetscCall(VecCUPMAllocateCheck_(v));
  if (auto &alloc = VecCUPMCast(v)->array_d) PetscFunctionReturn(0);
  else {
    const auto   n                 = v->map->n;
    auto        &array_allocated_d = VecCUPMCast(v)->array_allocated_d;
    cupmStream_t stream;

    PetscCall(GetHandlesFrom_(dctx, &stream));
    PetscCall(PetscCUPMMallocAsync(&array_allocated_d, n, stream));
    alloc = array_allocated_d;
    if (v->offloadmask == PETSC_OFFLOAD_UNALLOCATED) {
      const auto vimp = VecIMPLCast(v);
      v->offloadmask  = (vimp && vimp->array) ? PETSC_OFFLOAD_CPU : PETSC_OFFLOAD_GPU;
    }
  }
  PetscFunctionReturn(0);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::CopyToDevice_(PetscDeviceContext dctx, Vec v, bool forceasync) noexcept
{
  PetscFunctionBegin;
  PetscCall(DeviceAllocateCheck_(dctx, v));
  if (v->offloadmask == PETSC_OFFLOAD_CPU) {
    cupmStream_t stream;

    v->offloadmask = PETSC_OFFLOAD_BOTH;
    PetscCall(GetHandlesFrom_(dctx, &stream));
    PetscCall(PetscLogEventBegin(VEC_CUPMCopyToGPU(), v, 0, 0, 0));
    PetscCall(PetscCUPMMemcpyAsync(VecCUPMCast(v)->array_d, VecIMPLCast(v)->array, v->map->n, cupmMemcpyHostToDevice, stream, forceasync));
    PetscCall(PetscLogEventEnd(VEC_CUPMCopyToGPU(), v, 0, 0, 0));
  }
  PetscFunctionReturn(0);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::CopyToHost_(PetscDeviceContext dctx, Vec v, bool forceasync) noexcept
{
  PetscFunctionBegin;
  PetscCall(HostAllocateCheck_(dctx, v));
  if (v->offloadmask == PETSC_OFFLOAD_GPU) {
    cupmStream_t stream;

    v->offloadmask = PETSC_OFFLOAD_BOTH;
    PetscCall(GetHandlesFrom_(dctx, &stream));
    PetscCall(PetscLogEventBegin(VEC_CUPMCopyFromGPU(), v, 0, 0, 0));
    PetscCall(PetscCUPMMemcpyAsync(VecIMPLCast(v)->array, VecCUPMCast(v)->array_d, v->map->n, cupmMemcpyDeviceToHost, stream, forceasync));
    PetscCall(PetscLogEventEnd(VEC_CUPMCopyFromGPU(), v, 0, 0, 0));
  }
  PetscFunctionReturn(0);
}

// ==========================================================================================
// Vec_CUPMBase - Public API
// ==========================================================================================

template <device::cupm::DeviceType T, typename D>
inline typename Vec_CUPMBase<T, D>::Vec_CUPM *Vec_CUPMBase<T, D>::VecCUPMCast(Vec v) noexcept
{
  return static_cast<Vec_CUPM *>(v->spptr);
}

// This is a trick to get around the fact that in CRTP the derived class is not yet fully
// defined because Base<Derived> must necessarily be instantiated before Derived is
// complete. By using a dummy template parameter we make the type "dependent" and so will
// only be determined when the derived class is instantiated (and therefore fully defined)
template <device::cupm::DeviceType T, typename D>
template <typename U>
inline constexpr auto Vec_CUPMBase<T, D>::VecIMPLCast(Vec v) noexcept -> decltype(U::VecIMPLCast_(v))
{
  return U::VecIMPLCast_(v);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::VecDestroy_IMPL(Vec v) noexcept
{
  return D::VecDestroy_IMPL_(v);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::VecResetArray_IMPL(Vec v) noexcept
{
  return D::VecResetArray_IMPL_(v);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::VecPlaceArray_IMPL(Vec v, const PetscScalar *a) noexcept
{
  return D::VecPlaceArray_IMPL_(v, a);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::VecCreate_IMPL_Private(Vec v, PetscBool *alloc_missing, PetscInt nghost, PetscScalar *host_array) noexcept
{
  return D::VecCreate_IMPL_Private_(v, alloc_missing, nghost, host_array);
}

template <device::cupm::DeviceType T, typename D>
inline constexpr PetscLogEvent Vec_CUPMBase<T, D>::VEC_CUPMCopyToGPU() noexcept
{
  return T == device::cupm::DeviceType::CUDA ? VEC_CUDACopyToGPU : VEC_HIPCopyToGPU;
}

template <device::cupm::DeviceType T, typename D>
inline constexpr PetscLogEvent Vec_CUPMBase<T, D>::VEC_CUPMCopyFromGPU() noexcept
{
  return T == device::cupm::DeviceType::CUDA ? VEC_CUDACopyFromGPU : VEC_HIPCopyFromGPU;
}

template <device::cupm::DeviceType T, typename D>
inline constexpr VecType Vec_CUPMBase<T, D>::VECSEQCUPM() noexcept
{
  return T == device::cupm::DeviceType::CUDA ? VECSEQCUDA : VECSEQHIP;
}

template <device::cupm::DeviceType T, typename D>
inline constexpr VecType Vec_CUPMBase<T, D>::VECMPICUPM() noexcept
{
  return T == device::cupm::DeviceType::CUDA ? VECMPICUDA : VECMPIHIP;
}

template <device::cupm::DeviceType T, typename D>
template <typename U>
inline constexpr VecType Vec_CUPMBase<T, D>::VECIMPLCUPM() noexcept
{
  return U::VECIMPLCUPM_();
}

template <device::cupm::DeviceType T, typename D>
inline constexpr PetscRandomType Vec_CUPMBase<T, D>::PETSCDEVICERAND() noexcept
{
  // REVIEW ME: HIP default rng?
  return T == device::cupm::DeviceType::CUDA ? PETSCCURAND : PETSCRANDER48;
}

// utility for using cupmHostAlloc()
template <device::cupm::DeviceType T, typename D>
inline UseCUPMHostAlloc_<T> Vec_CUPMBase<T, D>::UseCUPMHostAlloc(bool b) noexcept
{
  return {b};
}

template <device::cupm::DeviceType T, typename D>
inline UseCUPMHostAlloc_<T> Vec_CUPMBase<T, D>::UseCUPMHostAlloc(PetscBool b) noexcept
{
  return UseCUPMHostAlloc(static_cast<bool>(b));
}

// private version that takes a PetscDeviceContext, called by the public variant
template <device::cupm::DeviceType T, typename D>
template <PetscMemType mtype, PetscMemoryAccessMode access, bool force>
inline PetscErrorCode Vec_CUPMBase<T, D>::getarray(Vec v, PetscScalar **a, PetscDeviceContext dctx) noexcept
{
  constexpr auto hostmem     = PetscMemTypeHost(mtype);
  const auto     oldmask     = v->offloadmask;
  auto          &mask        = v->offloadmask;
  auto           should_sync = false;

  PetscFunctionBegin;
  static_assert((mtype == PETSC_MEMTYPE_HOST) || (mtype == PETSC_MEMTYPE_DEVICE), "");
  PetscCheckTypeNames(v, VECSEQCUPM(), VECMPICUPM());
  if (PetscMemoryAccessRead(access)) {
    // READ or READ_WRITE
    if (((oldmask == PETSC_OFFLOAD_GPU) && hostmem) || ((oldmask == PETSC_OFFLOAD_CPU) && !hostmem)) {
      // if we move the data we should set the flag to synchronize later on
      should_sync = true;
    }
    PetscCall((hostmem ? CopyToHost_ : CopyToDevice_)(dctx, v, force));
  } else {
    // WRITE only
    PetscCall((hostmem ? HostAllocateCheck_ : DeviceAllocateCheck_)(dctx, v));
  }
  *a = hostmem ? VecIMPLCast(v)->array : VecCUPMCast(v)->array_d;
  // if unallocated previously we should zero things out if we intend to read
  if (PetscMemoryAccessRead(access) && (oldmask == PETSC_OFFLOAD_UNALLOCATED)) {
    const auto n = v->map->n;

    if (hostmem) {
      PetscCall(PetscArrayzero(*a, n));
    } else {
      cupmStream_t stream;

      PetscCall(GetHandlesFrom_(dctx, &stream));
      PetscCall(PetscCUPMMemsetAsync(*a, 0, n, stream, force));
      should_sync = true;
    }
  }
  // update the offloadmask if we intend to write, since we assume immediately modified
  if (PetscMemoryAccessWrite(access)) {
    PetscCall(VecSetErrorIfLocked(v, 1));
    // REVIEW ME: this should probably also call PetscObjectStateIncrease() since we assume it
    // is immediately modified
    mask = hostmem ? PETSC_OFFLOAD_CPU : PETSC_OFFLOAD_GPU;
  }
  // if we are a globally blocking stream and we have MOVED data then we should synchronize,
  // since even doing async calls on the NULL stream is not synchronous
  if (!force && should_sync) PetscCall(PetscDeviceContextSynchronize(dctx));
  PetscFunctionReturn(0);
}

// v->ops->getarray[read|write] or VecCUPMGetArray[Read|Write]()
template <device::cupm::DeviceType T, typename D>
template <PetscMemType mtype, PetscMemoryAccessMode access, bool force>
inline PetscErrorCode Vec_CUPMBase<T, D>::getarray(Vec v, PetscScalar **a) noexcept
{
  PetscDeviceContext dctx;

  PetscFunctionBegin;
  PetscCall(GetHandles_(&dctx));
  PetscCall(getarray<mtype, access, force>(v, a, dctx));
  PetscFunctionReturn(0);
}

// private version that takes a PetscDeviceContext, called by the public variant
template <device::cupm::DeviceType T, typename D>
template <PetscMemType mtype, PetscMemoryAccessMode access>
inline PetscErrorCode Vec_CUPMBase<T, D>::restorearray(Vec v, PetscScalar **a, PetscDeviceContext) noexcept
{
  PetscFunctionBegin;
  static_assert((mtype == PETSC_MEMTYPE_HOST) || (mtype == PETSC_MEMTYPE_DEVICE), "");
  PetscCheckTypeNames(v, VECSEQCUPM(), VECMPICUPM());
  if (PetscMemoryAccessWrite(access)) {
    // WRITE or READ_WRITE
    PetscCall(PetscObjectStateIncrease(PetscObjectCast(v)));
    v->offloadmask = PetscMemTypeHost(mtype) ? PETSC_OFFLOAD_CPU : PETSC_OFFLOAD_GPU;
  }
  if (a) {
    PetscCall(CheckPointerMatchesMemType_(*a, mtype));
    *a = nullptr;
  }
  PetscFunctionReturn(0);
}

// v->ops->restorearray[read|write] or VecCUPMRestoreArray[Read|Write]()
template <device::cupm::DeviceType T, typename D>
template <PetscMemType mtype, PetscMemoryAccessMode access>
inline PetscErrorCode Vec_CUPMBase<T, D>::restorearray(Vec v, PetscScalar **a) noexcept
{
  PetscDeviceContext dctx;

  PetscFunctionBegin;
  PetscCall(GetHandles_(&dctx));
  PetscCall(restorearray<mtype, access>(v, a, dctx));
  PetscFunctionReturn(0);
}

template <device::cupm::DeviceType T, typename D>
template <PetscMemoryAccessMode access>
inline PetscErrorCode Vec_CUPMBase<T, D>::getarrayandmemtype(Vec v, PetscScalar **a, PetscMemType *mtype, PetscDeviceContext dctx) noexcept
{
  PetscFunctionBegin;
  PetscCall(getarray<PETSC_MEMTYPE_DEVICE, access>(v, a, dctx));
  if (mtype) *mtype = (PetscDefined(HAVE_NVSHMEM) && VecCUPMCast(v)->nvshmem) ? PETSC_MEMTYPE_NVSHMEM : PETSC_MEMTYPE_CUPM();
  PetscFunctionReturn(0);
}

// v->ops->getarrayandmemtype
template <device::cupm::DeviceType T, typename D>
template <PetscMemoryAccessMode access>
inline PetscErrorCode Vec_CUPMBase<T, D>::getarrayandmemtype(Vec v, PetscScalar **a, PetscMemType *mtype) noexcept
{
  PetscDeviceContext dctx;

  PetscFunctionBegin;
  PetscCall(GetHandles_(&dctx));
  PetscCall(getarrayandmemtype<access>(v, a, mtype, dctx));
  PetscFunctionReturn(0);
}

template <device::cupm::DeviceType T, typename D>
template <PetscMemoryAccessMode access>
inline PetscErrorCode Vec_CUPMBase<T, D>::restorearrayandmemtype(Vec v, PetscScalar **a, PetscDeviceContext dctx) noexcept
{
  PetscFunctionBegin;
  PetscCall(restorearray<PETSC_MEMTYPE_DEVICE, access>(v, a, dctx));
  PetscFunctionReturn(0);
}

// v->ops->restorearrayandmemtype
template <device::cupm::DeviceType T, typename D>
template <PetscMemoryAccessMode access>
inline PetscErrorCode Vec_CUPMBase<T, D>::restorearrayandmemtype(Vec v, PetscScalar **a) noexcept
{
  PetscDeviceContext dctx;

  PetscFunctionBegin;
  PetscCall(GetHandles_(&dctx));
  PetscCall(restorearrayandmemtype<access>(v, a, dctx));
  PetscFunctionReturn(0);
}

// v->ops->placearray or VecCUPMPlaceArray()
template <device::cupm::DeviceType T, typename D>
template <PetscMemType mtype>
inline PetscErrorCode Vec_CUPMBase<T, D>::placearray(Vec v, const PetscScalar *a) noexcept
{
  PetscDeviceContext dctx;

  PetscFunctionBegin;
  static_assert((mtype == PETSC_MEMTYPE_HOST) || (mtype == PETSC_MEMTYPE_DEVICE), "");
  PetscCheckTypeNames(v, VECSEQCUPM(), VECMPICUPM());
  PetscCall(CheckPointerMatchesMemType_(a, mtype));
  PetscCall(GetHandles_(&dctx));
  if (PetscMemTypeHost(mtype)) {
    PetscCall(CopyToHost_(dctx, v));
    PetscCall(VecPlaceArray_IMPL(v, a));
    v->offloadmask = PETSC_OFFLOAD_CPU;
  } else {
    PetscCall(VecIMPLAllocateCheck_(v));
    {
      auto &backup_array = VecIMPLCast(v)->unplacedarray;

      PetscCheck(!backup_array, PETSC_COMM_SELF, PETSC_ERR_ARG_WRONGSTATE, "VecPlaceArray() was already called on this vector, without a call to VecResetArray()");
      PetscCall(CopyToDevice_(dctx, v));
      PetscCall(PetscObjectStateIncrease(PetscObjectCast(v)));
      backup_array = util::exchange(VecCUPMCast(v)->array_d, const_cast<PetscScalar *>(a));
      // only update the offload mask if we actually assign a pointer
      if (a) v->offloadmask = PETSC_OFFLOAD_GPU;
    }
  }
  PetscFunctionReturn(0);
}

// v->ops->replacearray or VecCUPMReplaceArray()
template <device::cupm::DeviceType T, typename D>
template <PetscMemType mtype>
inline PetscErrorCode Vec_CUPMBase<T, D>::replacearray(Vec v, const PetscScalar *a) noexcept
{
  const auto         aptr = const_cast<PetscScalar *>(a);
  PetscDeviceContext dctx;

  PetscFunctionBegin;
  static_assert((mtype == PETSC_MEMTYPE_HOST) || (mtype == PETSC_MEMTYPE_DEVICE), "");
  PetscCheckTypeNames(v, VECSEQCUPM(), VECMPICUPM());
  PetscCall(CheckPointerMatchesMemType_(a, mtype));
  PetscCall(GetHandles_(&dctx));
  if (PetscMemTypeHost(mtype)) {
    PetscCall(VecIMPLAllocateCheck_(v));
    {
      const auto vimpl      = VecIMPLCast(v);
      auto      &host_array = vimpl->array_allocated;

      // make sure the users array has the latest values.
      // REVIEW ME: why? we're about to free it
      if (host_array != vimpl->array) PetscCall(CopyToHost_(dctx, v));
      if (host_array) {
        const auto useit = UseCUPMHostAlloc(v->pinned_memory);

        PetscCall(PetscFree(host_array));
      }
      host_array       = aptr;
      vimpl->array     = host_array;
      v->pinned_memory = PETSC_FALSE; // REVIEW ME: we can determine this
      v->offloadmask   = PETSC_OFFLOAD_CPU;
    }
  } else {
    PetscCall(VecCUPMAllocateCheck_(v));
    {
      const auto vcu = VecCUPMCast(v);

      PetscCall(ResetAllocatedDevicePtr_(dctx, v, aptr));
      // don't update the offloadmask if placed pointer is NULL
      vcu->array_d = vcu->array_allocated_d /* = aptr */;
      if (aptr) v->offloadmask = PETSC_OFFLOAD_GPU;
    }
  }
  PetscCall(PetscObjectStateIncrease(PetscObjectCast(v)));
  PetscFunctionReturn(0);
}

// v->ops->resetarray or VecCUPMResetArray()
template <device::cupm::DeviceType T, typename D>
template <PetscMemType mtype>
inline PetscErrorCode Vec_CUPMBase<T, D>::resetarray(Vec v) noexcept
{
  PetscDeviceContext dctx;

  PetscFunctionBegin;
  static_assert((mtype == PETSC_MEMTYPE_HOST) || (mtype == PETSC_MEMTYPE_DEVICE), "");
  PetscCheckTypeNames(v, VECSEQCUPM(), VECMPICUPM());
  PetscCall(GetHandles_(&dctx));
  // REVIEW ME:
  // this is wildly inefficient but must be done if we assume that the placed array must have
  // correct values
  if (PetscMemTypeHost(mtype)) {
    PetscCall(CopyToHost_(dctx, v));
    PetscCall(VecResetArray_IMPL(v));
    v->offloadmask = PETSC_OFFLOAD_CPU;
  } else {
    PetscCall(VecIMPLAllocateCheck_(v));
    PetscCall(VecCUPMAllocateCheck_(v));
    {
      const auto vcu        = VecCUPMCast(v);
      const auto vimpl      = VecIMPLCast(v);
      auto      &host_array = vimpl->unplacedarray;

      PetscCall(CheckPointerMatchesMemType_(host_array, PETSC_MEMTYPE_DEVICE));
      PetscCall(CopyToDevice_(dctx, v));
      PetscCall(PetscObjectStateIncrease(PetscObjectCast(v)));
      // Need to reset the offloadmask. If we had a stashed pointer we are on the GPU,
      // otherwise check if the host has a valid pointer. If neither, then we are not
      // allocated.
      vcu->array_d = host_array;
      if (host_array) {
        host_array     = nullptr;
        v->offloadmask = PETSC_OFFLOAD_GPU;
      } else if (vimpl->array) {
        v->offloadmask = PETSC_OFFLOAD_CPU;
      } else {
        v->offloadmask = PETSC_OFFLOAD_UNALLOCATED;
      }
    }
  }
  PetscFunctionReturn(0);
}

// v->ops->create
template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::create(Vec v) noexcept
{
  PetscBool          alloc_missing;
  PetscDeviceContext dctx;

  PetscFunctionBegin;
  PetscCall(VecCreate_IMPL_Private(v, &alloc_missing));
  PetscCall(GetHandles_(&dctx));
  PetscCall(Initialize_CUPMBase(v, alloc_missing, nullptr, nullptr, dctx));
  PetscFunctionReturn(0);
}

// v->ops->destroy
template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::destroy(Vec v) noexcept
{
  PetscFunctionBegin;
  if (const auto vcu = VecCUPMCast(v)) {
    PetscDeviceContext dctx;

    PetscCall(GetHandles_(&dctx));
    PetscCall(ResetAllocatedDevicePtr_(dctx, v));
    PetscCall(ResetPreallocationCOO_CUPMBase(v, dctx));
    PetscCall(PetscFree(v->spptr));
  }
  PetscCall(PetscObjectSAWsViewOff(PetscObjectCast(v)));
  if (const auto vimpl = VecIMPLCast(v)) {
    if (auto &array_allocated = vimpl->array_allocated) {
      const auto useit = UseCUPMHostAlloc(v->pinned_memory);

      // do this ourselves since we may want to use the cupm functions
      PetscCall(PetscFree(array_allocated));
    }
  }
  v->pinned_memory = PETSC_FALSE;
  PetscCall(VecDestroy_IMPL(v));
  PetscFunctionReturn(0);
}

// ================================================================================== //
//                      Common core between Seq and MPI                               //

// VecCreate_CUPM()
template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::Create_CUPM(Vec v) noexcept
{
  PetscMPIInt size;

  PetscFunctionBegin;
  PetscCallMPI(MPI_Comm_size(PetscObjectComm(PetscObjectCast(v)), &size));
  PetscCall(VecSetType(v, size > 1 ? VECMPICUPM() : VECSEQCUPM()));
  PetscFunctionReturn(0);
}

// VecCreateCUPM()
template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::Create_CUPMBase(MPI_Comm comm, PetscInt bs, PetscInt n, PetscInt N, Vec *v, PetscBool call_set_type, PetscLayout reference) noexcept
{
  PetscFunctionBegin;
  PetscCall(VecCreate(comm, v));
  if (reference) PetscCall(PetscLayoutReference(reference, &(*v)->map));
  PetscCall(VecSetSizes(*v, n, N));
  if (bs) PetscCall(VecSetBlockSize(*v, bs));
  if (call_set_type) PetscCall(VecSetType(*v, VECIMPLCUPM()));
  PetscFunctionReturn(0);
}

// VecCreateIMPL_CUPM(), called through v->ops->create
template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::Initialize_CUPMBase(Vec v, PetscBool allocate_missing, PetscScalar *host_array, PetscScalar *device_array, PetscDeviceContext dctx) noexcept
{
  PetscFunctionBegin;
  // REVIEW ME: perhaps not needed
  PetscCall(PetscDeviceInitialize(PETSC_DEVICE_CUPM()));
  PetscCall(PetscObjectChangeTypeName(PetscObjectCast(v), VECIMPLCUPM()));
  PetscCall(D::bindtocpu(v, PETSC_FALSE));
  if (device_array) {
    PetscCall(CheckPointerMatchesMemType_(device_array, PETSC_MEMTYPE_CUPM()));
    PetscCall(VecCUPMAllocateCheck_(v));
    VecCUPMCast(v)->array_d = device_array;
  }
  if (host_array) {
    PetscCall(CheckPointerMatchesMemType_(host_array, PETSC_MEMTYPE_HOST));
    VecIMPLCast(v)->array = host_array;
  }
  if (allocate_missing) {
    PetscCall(DeviceAllocateCheck_(dctx, v));
    PetscCall(HostAllocateCheck_(dctx, v));
    // REVIEW ME: junchao, is this needed with new calloc() branch? VecSet() will call
    // set() for reference
    // calls device-version
    PetscCall(VecSet(v, 0));
    // zero the host while device is underway
    PetscCall(PetscArrayzero(VecIMPLCast(v)->array, v->map->n));
    v->offloadmask = PETSC_OFFLOAD_BOTH;
  } else {
    if (host_array) {
      v->offloadmask = device_array ? PETSC_OFFLOAD_BOTH : PETSC_OFFLOAD_CPU;
    } else {
      v->offloadmask = device_array ? PETSC_OFFLOAD_GPU : PETSC_OFFLOAD_UNALLOCATED;
    }
  }
  PetscFunctionReturn(0);
}

// v->ops->duplicate
template <device::cupm::DeviceType T, typename D>
template <typename SetupFunctionT>
inline PetscErrorCode Vec_CUPMBase<T, D>::Duplicate_CUPMBase(Vec v, Vec *y, PetscDeviceContext dctx, SetupFunctionT &&DerivedCreateIMPLCUPM_Async) noexcept
{
  // if the derived setup is the default no_op then we should call VecSetType()
  constexpr auto call_set_type = static_cast<PetscBool>(std::is_same<SetupFunctionT, no_op>::value);
  const auto     vobj          = PetscObjectCast(v);
  const auto     map           = v->map;
  PetscInt       bs;

  PetscFunctionBegin;
  PetscCall(VecGetBlockSize(v, &bs));
  PetscCall(Create_CUPMBase(PetscObjectComm(vobj), bs, map->n, map->N, y, call_set_type, map));
  // Derived class can set up the remainder of the data structures here
  PetscCall(DerivedCreateIMPLCUPM_Async(*y));
  // If the other vector is bound to CPU then the memcpy of the ops struct will give the
  // duplicated vector the host "getarray" function which does not lazily allocate the array
  // (as it is assumed to always exist). So we force allocation here, before we overwrite the
  // ops
  if (v->boundtocpu) PetscCall(HostAllocateCheck_(dctx, *y));
  // in case the user has done some VecSetOps() tomfoolery
  PetscCall(PetscArraycpy((*y)->ops, v->ops, 1));
  {
    const auto yobj = PetscObjectCast(*y);

    PetscCall(PetscObjectListDuplicate(vobj->olist, &yobj->olist));
    PetscCall(PetscFunctionListDuplicate(vobj->qlist, &yobj->qlist));
  }
  (*y)->stash.donotstash   = v->stash.donotstash;
  (*y)->stash.ignorenegidx = v->stash.ignorenegidx;
  (*y)->map->bs            = std::abs(v->map->bs);
  (*y)->bstash.bs          = v->bstash.bs;
  PetscFunctionReturn(0);
}

  #define VecSetOp_CUPM(op_name, op_host, ...) \
    do { \
      if (usehost) { \
        v->ops->op_name = op_host; \
      } else { \
        v->ops->op_name = __VA_ARGS__; \
      } \
    } while (0)

// v->ops->bindtocpu
template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::BindToCPU_CUPMBase(Vec v, PetscBool usehost, PetscDeviceContext dctx) noexcept
{
  const auto change_default_rand_type = [](PetscRandomType target, char **ptr) {
    PetscFunctionBegin;
    PetscValidPointer(ptr, 2);
    PetscValidCharPointer(*ptr, 2);
    if (std::strcmp(target, *ptr)) {
      PetscCall(PetscFree(*ptr));
      PetscCall(PetscStrallocpy(target, ptr));
    }
    PetscFunctionReturn(0);
  };

  PetscFunctionBegin;
  v->boundtocpu = usehost;
  if (usehost) PetscCall(CopyToHost_(dctx, v));
  PetscCall(change_default_rand_type(usehost ? PETSCRANDER48 : PETSCDEVICERAND(), &v->defaultrandtype));

  // set the base functions that are guaranteed to be the same for both
  v->ops->duplicate = D::duplicate;
  v->ops->create    = create;
  v->ops->destroy   = destroy;
  v->ops->bindtocpu = D::bindtocpu;
  // Note that setting these to NULL on host breaks convergence in certain areas. I don't know
  // why, and I don't know how, but it is IMPERATIVE these are set as such!
  v->ops->replacearray = replacearray<PETSC_MEMTYPE_HOST>;
  v->ops->restorearray = restorearray<PETSC_MEMTYPE_HOST, PETSC_MEMORY_ACCESS_READ_WRITE>;

  // set device-only common functions
  VecSetOp_CUPM(dotnorm2, nullptr, D::dotnorm2);
  VecSetOp_CUPM(getarray, nullptr, getarray<PETSC_MEMTYPE_HOST, PETSC_MEMORY_ACCESS_READ_WRITE>);
  VecSetOp_CUPM(getarraywrite, nullptr, getarray<PETSC_MEMTYPE_HOST, PETSC_MEMORY_ACCESS_WRITE>);
  VecSetOp_CUPM(restorearraywrite, nullptr, restorearray<PETSC_MEMTYPE_HOST, PETSC_MEMORY_ACCESS_WRITE>);

  VecSetOp_CUPM(getarrayread, nullptr, [](Vec v, const PetscScalar **a) { return getarray<PETSC_MEMTYPE_HOST, PETSC_MEMORY_ACCESS_READ>(v, const_cast<PetscScalar **>(a)); });
  VecSetOp_CUPM(restorearrayread, nullptr, [](Vec v, const PetscScalar **a) { return restorearray<PETSC_MEMTYPE_HOST, PETSC_MEMORY_ACCESS_READ>(v, const_cast<PetscScalar **>(a)); });

  VecSetOp_CUPM(getarrayandmemtype, nullptr, getarrayandmemtype<PETSC_MEMORY_ACCESS_READ_WRITE>);
  VecSetOp_CUPM(restorearrayandmemtype, nullptr, restorearrayandmemtype<PETSC_MEMORY_ACCESS_READ_WRITE>);

  VecSetOp_CUPM(getarraywriteandmemtype, nullptr, getarrayandmemtype<PETSC_MEMORY_ACCESS_WRITE>);
  VecSetOp_CUPM(restorearraywriteandmemtype, nullptr, [](Vec v, PetscScalar **a, PetscMemType *) { return restorearrayandmemtype<PETSC_MEMORY_ACCESS_WRITE>(v, a); });

  VecSetOp_CUPM(getarrayreadandmemtype, nullptr, [](Vec v, const PetscScalar **a, PetscMemType *m) { return getarrayandmemtype<PETSC_MEMORY_ACCESS_READ>(v, const_cast<PetscScalar **>(a), m); });
  VecSetOp_CUPM(restorearrayreadandmemtype, nullptr, [](Vec v, const PetscScalar **a) { return restorearrayandmemtype<PETSC_MEMORY_ACCESS_READ>(v, const_cast<PetscScalar **>(a)); });

  // set the functions that are always sequential
  using VecSeq_T = VecSeq_CUPM<T>;
  VecSetOp_CUPM(scale, VecScale_Seq, VecSeq_T::scale);
  VecSetOp_CUPM(copy, VecCopy_Seq, VecSeq_T::copy);
  VecSetOp_CUPM(set, VecSet_Seq, VecSeq_T::set);
  VecSetOp_CUPM(swap, VecSwap_Seq, VecSeq_T::swap);
  VecSetOp_CUPM(axpy, VecAXPY_Seq, VecSeq_T::axpy);
  VecSetOp_CUPM(axpby, VecAXPBY_Seq, VecSeq_T::axpby);
  VecSetOp_CUPM(maxpy, VecMAXPY_Seq, VecSeq_T::maxpy);
  VecSetOp_CUPM(aypx, VecAYPX_Seq, VecSeq_T::aypx);
  VecSetOp_CUPM(waxpy, VecWAXPY_Seq, VecSeq_T::waxpy);
  VecSetOp_CUPM(axpbypcz, VecAXPBYPCZ_Seq, VecSeq_T::axpbypcz);
  VecSetOp_CUPM(pointwisemult, VecPointwiseMult_Seq, VecSeq_T::pointwisemult);
  VecSetOp_CUPM(pointwisedivide, VecPointwiseDivide_Seq, VecSeq_T::pointwisedivide);
  VecSetOp_CUPM(setrandom, VecSetRandom_Seq, VecSeq_T::setrandom);
  VecSetOp_CUPM(dot_local, VecDot_Seq, VecSeq_T::dot);
  VecSetOp_CUPM(tdot_local, VecTDot_Seq, VecSeq_T::tdot);
  VecSetOp_CUPM(norm_local, VecNorm_Seq, VecSeq_T::norm);
  VecSetOp_CUPM(mdot_local, VecMDot_Seq, VecSeq_T::mdot);
  VecSetOp_CUPM(reciprocal, VecReciprocal_Default, VecSeq_T::reciprocal);
  VecSetOp_CUPM(shift, nullptr, VecSeq_T::shift);
  VecSetOp_CUPM(getlocalvector, nullptr, VecSeq_T::template getlocalvector<PETSC_MEMORY_ACCESS_READ_WRITE>);
  VecSetOp_CUPM(restorelocalvector, nullptr, VecSeq_T::template restorelocalvector<PETSC_MEMORY_ACCESS_READ_WRITE>);
  VecSetOp_CUPM(getlocalvectorread, nullptr, VecSeq_T::template getlocalvector<PETSC_MEMORY_ACCESS_READ>);
  VecSetOp_CUPM(restorelocalvectorread, nullptr, VecSeq_T::template restorelocalvector<PETSC_MEMORY_ACCESS_READ>);
  VecSetOp_CUPM(sum, nullptr, VecSeq_T::sum);
  PetscFunctionReturn(0);
}

// Called from VecGetSubVector()
template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::GetArrays_CUPMBase(Vec v, const PetscScalar **host_array, const PetscScalar **device_array, PetscOffloadMask *mask, PetscDeviceContext dctx) noexcept
{
  PetscFunctionBegin;
  PetscCheckTypeNames(v, VECSEQCUPM(), VECMPICUPM());
  if (host_array) {
    PetscCall(HostAllocateCheck_(dctx, v));
    *host_array = VecIMPLCast(v)->array;
  }
  if (device_array) {
    PetscCall(DeviceAllocateCheck_(dctx, v));
    *device_array = VecCUPMCast(v)->array_d;
  }
  if (mask) *mask = v->offloadmask;
  PetscFunctionReturn(0);
}

template <device::cupm::DeviceType T, typename D>
inline PetscErrorCode Vec_CUPMBase<T, D>::ResetPreallocationCOO_CUPMBase(Vec v, PetscDeviceContext dctx) noexcept
{
  PetscFunctionBegin;
  if (const auto vcu = VecCUPMCast(v)) {
    cupmStream_t stream;
    // clang-format off
    const auto   cntptrs = util::make_array(
      std::ref(vcu->jmap1_d),
      std::ref(vcu->perm1_d),
      std::ref(vcu->imap2_d),
      std::ref(vcu->jmap2_d),
      std::ref(vcu->perm2_d),
      std::ref(vcu->Cperm_d)
    );
    // clang-format on

    PetscCall(GetHandlesFrom_(dctx, &stream));
    for (auto &&ptr : cntptrs) PetscCallCUPM(cupmFreeAsync(ptr.get(), stream));
    for (auto &&ptr : util::make_array(std::ref(vcu->sendbuf_d), std::ref(vcu->recvbuf_d))) PetscCallCUPM(cupmFreeAsync(ptr.get(), stream));
  }
  PetscFunctionReturn(0);
}

template <device::cupm::DeviceType T, typename D>
template <std::size_t NCount, std::size_t NScal>
inline PetscErrorCode Vec_CUPMBase<T, D>::SetPreallocationCOO_CUPMBase(Vec v, PetscCount ncoo, const PetscInt coo_i[], PetscDeviceContext dctx, const std::array<CooPair<PetscCount>, NCount> &extra_cntptrs, const std::array<CooPair<PetscScalar>, NScal> &bufptrs) noexcept
{
  const auto vimpl = VecIMPLCast(v);

  PetscFunctionBegin;
  PetscCall(ResetPreallocationCOO_CUPMBase(v, dctx));
  // need to instantiate the private pointer if not already
  PetscCall(VecCUPMAllocateCheck_(v));
  {
    const auto vcu = VecCUPMCast(v);
    // clang-fomat off
    const auto cntptrs = util::concat_array(util::make_array(make_coo_pair(vcu->jmap1_d, vimpl->jmap1, v->map->n + 1), make_coo_pair(vcu->perm1_d, vimpl->perm1, vimpl->tot1)), extra_cntptrs);
    // clang-format on
    cupmStream_t stream;

    PetscCall(GetHandlesFrom_(dctx, &stream));
    // allocate
    for (auto &elem : cntptrs) PetscCall(PetscCUPMMallocAsync(&elem.device, elem.size, stream));
    for (auto &elem : bufptrs) PetscCall(PetscCUPMMallocAsync(&elem.device, elem.size, stream));
    // copy
    for (const auto &elem : cntptrs) PetscCall(PetscCUPMMemcpyAsync(elem.device, elem.host, elem.size, cupmMemcpyHostToDevice, stream, true));
    for (const auto &elem : bufptrs) PetscCall(PetscCUPMMemcpyAsync(elem.device, elem.host, elem.size, cupmMemcpyHostToDevice, stream, true));
  }
  PetscFunctionReturn(0);
}

  #define PETSC_VEC_CUPM_BASE_CLASS_HEADER(name, Tp, ...) \
    using name = ::Petsc::vec::cupm::impl::Vec_CUPMBase<Tp, __VA_ARGS__>; \
    friend name; \
    /* introspection */ \
    using name::VecCUPMCast; \
    using name::VecIMPLCast; \
    using name::VECIMPLCUPM; \
    using name::VECSEQCUPM; \
    using name::VECMPICUPM; \
    using name::VecView_Debug; \
    /* utility */ \
    using typename name::Vec_CUPM; \
    using name::UseCUPMHostAlloc; \
    using name::GetHandles_; \
    using name::GetHandlesFrom_; \
    using name::VecCUPMAllocateCheck_; \
    using name::VecIMPLAllocateCheck_; \
    using name::HostAllocateCheck_; \
    using name::DeviceAllocateCheck_; \
    using name::CopyToDevice_; \
    using name::CopyToHost_; \
    using name::create; \
    using name::destroy; \
    using name::getarray; \
    using name::restorearray; \
    using name::getarrayandmemtype; \
    using name::restorearrayandmemtype; \
    using name::placearray; \
    using name::replacearray; \
    using name::resetarray; \
    /* base functions */ \
    using name::Create_CUPMBase; \
    using name::Initialize_CUPMBase; \
    using name::Duplicate_CUPMBase; \
    using name::BindToCPU_CUPMBase; \
    using name::Create_CUPM; \
    using name::DeviceArrayRead; \
    using name::DeviceArrayWrite; \
    using name::DeviceArrayReadWrite; \
    using name::HostArrayRead; \
    using name::HostArrayWrite; \
    using name::HostArrayReadWrite; \
    using name::ResetPreallocationCOO_CUPMBase; \
    using name::SetPreallocationCOO_CUPMBase; \
    /* blas interface */ \
    PETSC_CUPMBLAS_INHERIT_INTERFACE_TYPEDEFS_USING(cupmBlasInterface_t, Tp)

} // namespace impl

} // namespace cupm

} // namespace vec

} // namespace Petsc

#endif // __cplusplus && PetscDefined(HAVE_DEVICE)

#endif // PETSCVECCUPMIMPL_H
