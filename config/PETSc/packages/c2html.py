import config.package
import os

class Configure(config.package.GNUPackage):
  def __init__(self, framework):
    config.package.GNUPackage.__init__(self, framework)
    self.download          = ['http://ftp.mcs.anl.gov/pub/petsc/externalpackages/c2html.tar.gz']
    self.complex           = 1
    self.double            = 0
    self.requires32bitint  = 0
    self.downloadonWindows = 1
    self.publicInstall     = 0  # always install in PETSC_DIR/PETSC_ARCH (not --prefix) since this is not used by users
    self.parallelMake      = 0  # sowing does not support make -j np

  def setupHelp(self, help):
    import nargs
    config.package.GNUPackage.setupHelp(self, help)
    help.addArgument('C2html', '-download-c2html-cc=<prog>',                     nargs.Arg(None, None, 'C compiler for c2html'))
    help.addArgument('C2html', '-download-c2html-configure-options=<options>',   nargs.Arg(None, None, 'additional options for c2html'))
    return

  def setupDependencies(self, framework):
    config.package.GNUPackage.setupDependencies(self, framework)
    self.petscclone     = framework.require('PETSc.utilities.petscclone',self.setCompilers)
    return

  def formGNUConfigureArgs(self):
    '''Does not use the standard arguments at all since this does not use the MPI compilers etc
       Sowing will chose its own compilers if they are not provided explicitly here'''
    args = ['--prefix='+self.confDir]
    if 'download-c2html-cc' in self.framework.argDB and self.framework.argDB['download-c2html-cc']:
      args.append('CC="'+self.framework.argDB['download-c2html-cc']+'"')
    if 'download-c2html-configure-options' in self.framework.argDB and self.framework.argDB['download-c2html-configure-options']:
      args.append(self.framework.argDB['download-c2html-configure-options'])
    return args

  def Install(self):
    # check if flex or lex are in PATH
    self.getExecutable('flex')
    self.getExecutable('lex')
    if not hasattr(self, 'flex') and not hasattr(self, 'lex'):
      raise RuntimeError('Cannot build c2html. It requires either "flex" or "lex" in PATH. Please install flex and retry.\nOr disable c2html with --with-c2html=0')
    return config.package.GNUPackage.Install(self)

  def alternateConfigureLibrary(self):
    self.checkDownload(1)

  def configure(self):
    if (self.framework.clArgDB.has_key('with-c2html') and not self.framework.argDB['with-c2html']) or \
          (self.framework.clArgDB.has_key('download-c2html') and not self.framework.argDB['download-c2html']):
      self.framework.logPrint("Not checking c2html on user request\n")
      return

    if self.petscclone.isClone:
      self.framework.logPrint('PETSc clone, checking for c2html\n')
      self.getExecutable('c2html', getFullPath = 1)

      if hasattr(self, 'c2html') and not self.framework.argDB.get('download-c2html'):
        self.framework.logPrint('Found c2html, will not install c2html')
      else:
        self.framework.logPrint('Installing c2html')
        if not self.framework.argDB.get('download-c2html'): self.framework.argDB['download-c2html'] = 1
        config.package.GNUPackage.configure(self)
        self.getExecutable('c2html',    path=os.path.join(self.installDir,'bin'), getFullPath = 1)
    else:
      self.framework.logPrint("Not a clone of PETSc, don't need c2html\n")
    return
