add_flang_library(FIRTransforms
  AbstractResult.cpp
  AffinePromotion.cpp
  AffineDemotion.cpp
  CharacterConversion.cpp
  ArrayValueCopy.cpp
  Inliner.cpp
  ExternalNameConversion.cpp
  MemRefDataFlowOpt.cpp
  RewriteLoop.cpp

  DEPENDS
  FIRBuilder
  FIRDialect
  FIRSupport
  FIROptTransformsPassIncGen

  LINK_LIBS
  FIRBuilder
  FIRDialect
  MLIRAffineToStandard
  MLIRLLVMIR
  MLIROpenACC
  MLIROpenMP
  FIRSupport
)
