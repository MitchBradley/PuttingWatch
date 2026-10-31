use core::ops::{Add, Mul, Sub};

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Vec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl Vec3 {
    pub const fn new(x: f32, y: f32, z: f32) -> Self {
        Vec3 { x, y, z }
    }

    pub fn dot(self, o: Vec3) -> f32 {
        self.x * o.x + self.y * o.y + self.z * o.z
    }

    pub fn cross(self, o: Vec3) -> Vec3 {
        Vec3::new(
            self.y * o.z - self.z * o.y,
            self.z * o.x - self.x * o.z,
            self.x * o.y - self.y * o.x,
        )
    }

    pub fn norm(self) -> f32 {
        self.dot(self).sqrt()
    }

    pub fn normalized(self) -> Vec3 {
        let n = self.norm();
        if n > 1e-9 {
            Vec3::new(self.x / n, self.y / n, self.z / n)
        } else {
            Vec3::default()
        }
    }
}

impl Add for Vec3 {
    type Output = Vec3;
    fn add(self, o: Vec3) -> Vec3 {
        Vec3::new(self.x + o.x, self.y + o.y, self.z + o.z)
    }
}

impl Sub for Vec3 {
    type Output = Vec3;
    fn sub(self, o: Vec3) -> Vec3 {
        Vec3::new(self.x - o.x, self.y - o.y, self.z - o.z)
    }
}

impl Mul<f32> for Vec3 {
    type Output = Vec3;
    fn mul(self, s: f32) -> Vec3 {
        Vec3::new(self.x * s, self.y * s, self.z * s)
    }
}

/// Angle between two vectors, in degrees. The `acos` argument is clamped first: float rounding
/// can push the ratio a hair past 1 for near-identical vectors, which would give NaN.
pub fn angle_between_degrees(a: Vec3, b: Vec3) -> f32 {
    let (na, nb) = (a.norm(), b.norm());
    if na < 1e-9 || nb < 1e-9 {
        return 0.0;
    }
    let c = (a.dot(b) / (na * nb)).clamp(-1.0, 1.0);
    c.acos() * 57.295_78
}
