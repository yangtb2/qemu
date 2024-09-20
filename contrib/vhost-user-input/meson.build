executable('vhost-user-input', files('vhost-user-input.c', 'main.c'),
           dependencies: [qemuutil, vhost_user],
           build_by_default: host_os == 'linux',
           install: false)
