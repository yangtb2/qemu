executable('vhost-user-input-collector', files('main.c', 'vhost-user-input-host.c'),
            c_args: ['-fPIC'],
            dependencies: [qemuutil, vhost_user],
            install: true)